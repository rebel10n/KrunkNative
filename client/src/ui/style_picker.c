// v10.0.7 owned-item style picker. Selection belongs to a stack or a specific painted copy. v10 posts items.setStyle
// (POST /items/<skinId>/style {item_id, style}, deob-10.0.7.js:27101-27106, :108817-108850); the native client sends
// the same request over the brain's social socket, ["ist", skinId, item_id, style, uid] -> ["istr", {copy_id, item_id,
// style} | null, uid] or "po" (services/brain/economy.c sh_item_style; the brain's HTTP route runs the same op).
// A krunker.io account sends v10.0.7's request itself to krunker.io with its Bearer (kio_store_write), skinId the card's
// instance; the answer is applied as v10 applies it (complete).
#include <style_picker.h>
#include <kio_store.h> // kio_store_write: a krunker.io account's setStyle
#include <skin_style.h>
#include <backend_ui.h>
#include <backend.h>
#include <ui_logic.h>
#include <v10_ui.h>
#include <v10_menu.h>
#include <client.h>
#include <net_ext.h>
#include "../viewer/viewer_store.h"
#include <cJSON.h>
#include <stdint.h> // intptr_t: the style of a krunker.io request
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STYLE_WAIT_MS 15000
// The swatch row: v10 scrolls it by 0.8 * clientWidth per arrow (deob-10.0.7.js:108878-108887); the native page has no
// scrollBy, so the arrows page through it STYLE_PAGE_SIZE swatches at a time (88px + 10px gap each in the 700px popup).
#define STYLE_PAGE_SIZE 5
#define COPY_ID_BASE 1000000

static struct {
    int open, busy, index, sid, selected, page, uid, timer;
    char error[160];
} S;
static int g_listening, g_uids;

static int picker_arg(const HtmlArg *args, int argc, int k, int fallback) {
    if (k < 0 || k >= argc) return fallback;
    return args[k].kind == HTML_ARG_NUM || args[k].kind == HTML_ARG_BOOL ? (int) args[k].num : fallback;
}

static const cJSON *owned_list(void) {
    const AccountInfo *acc = active_account();
    return acc && acc->data ? cJSON_GetArrayItem(acc->data, 10) : NULL;
}

// 1 when sid names an instance of the owned list: a copy id (sid >= COPY_ID_BASE) on the brain's account, an entry's id
// on krunker.io's (its instance ids are not in the brain's copy id space)
static int is_instance(const cJSON *owned, const int sid) {
    if (account_backend() != ACCOUNT_BACKEND_KRUNKER) return sid >= COPY_ID_BASE;
    const cJSON *row;
    cJSON_ArrayForEach(row, owned) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(row, "id");
        if (cJSON_IsNumber(id) && id->valueint == sid) return 1;
    }
    return 0;
}

static int owned_style(int index, int sid) {
    const cJSON *owned = owned_list(), *row;
    const int instance = is_instance(owned, sid);
    cJSON_ArrayForEach(row, owned) {
        const cJSON *ind = cJSON_GetObjectItemCaseSensitive(row, "ind");
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(row, "id");
        if (!cJSON_IsNumber(ind) || ind->valueint != index) continue;
        if (instance ? !cJSON_IsNumber(id) || id->valueint != sid : cJSON_IsNumber(id)) continue;
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(row, "m");
        cJSON *parsed = NULL;
        if (cJSON_IsString(m) && m->valuestring) m = parsed = cJSON_Parse(m->valuestring);
        const cJSON *style = cJSON_GetObjectItemCaseSensitive(m, "style");
        const int value = cJSON_IsNumber(style) ? style->valueint : 0;
        cJSON_Delete(parsed);
        return value;
    }
    return 0;
}

int style_picker_account_style(int index) {
    const cJSON *owned = owned_list(), *row;
    cJSON_ArrayForEach(row, owned) {
        const cJSON *ind = cJSON_GetObjectItemCaseSensitive(row, "ind");
        if (!cJSON_IsNumber(ind) || ind->valueint != index) continue;
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(row, "id");
        const int style = owned_style(index, cJSON_IsNumber(id) ? id->valueint : index);
        if (style > 0) return style;
    }
    return 0;
}

int style_picker_can_open(int index) {
    const ViewerSkin *item = viewer_skin(index);
    // styles.length > 0 (deob-10.0.7.js:135291, :147912)
    return item && item->styles > 0 && active_account(); // (krunker.io too: apply sends its setStyle to krunker.io)
}

// styles[i] || "Style " + i (deob-10.0.7.js:108866-108870)
static void style_name(char *out, const size_t n, const int i) {
    const cJSON *styles = cJSON_GetObjectItemCaseSensitive(ui_loadout_skin_json(S.index), "styles");
    const char *name = cJSON_GetStringValue(cJSON_GetArrayItem(styles, i));
    if (name && *name) snprintf(out, n, "%s", name);
    else snprintf(out, n, "Style %d", i);
}

// The style popup (deob-10.0.7.js:108851-108856, :108938-108990) in the paint picker's popup wrapper. Not drawn: the
// style-iframe viewer.html?nobg&showcase=<index>&style=<n> and so the style-overlay over it (the native page has no
// iframes): the header and controls flow in the content, which keeps their height instead of 800px.
static char *style_html(void) {
    const ViewerSkin *item = viewer_skin(S.index);
    StrBuf sb = {0};
    if (!item) return sb_finish(&sb);
    char name[160];
    sb_append(&sb, "<div class='popup-container svelte-1wz7ymp'><div class='popup svelte-1wz7ymp' style='width:700px'>"
                   "<div class='style-container io-popup-container svelte-143xo7r'><div class='style-content io-popup-content svelte-143xo7r' style='height:auto'>"
                   "<div class='style-header svelte-143xo7r'><div class='item-name svelte-143xo7r'>");
    sb_append_esc(&sb, item->name && *item->name ? item->name : "Unnamed Item");
    sb_append(&sb, "</div> <div class='item-sub svelte-143xo7r'>Pick a style to preview, then press Apply.</div></div> "
                   "<div class='style-controls svelte-143xo7r'>");
    if (item->styles <= 0) {
        sb_append(&sb, "<div class='style-empty svelte-143xo7r'>This item has no styles.</div>");
    } else {
        const int first = S.page * STYLE_PAGE_SIZE;
        const int end = first + STYLE_PAGE_SIZE < item->styles ? first + STYLE_PAGE_SIZE : item->styles;
        sb_appendf(&sb, "<div class='swatch-slider svelte-143xo7r'><button type='button' class='slider-arrow left svelte-143xo7r%s' aria-label='Scroll styles left'"
                        " onclick='stylePage(-1)'%s><span class='slider-arrow-icon svelte-143xo7r'></span></button> "
                        "<div class='swatch-row svelte-143xo7r' role='radiogroup' aria-label='Available styles'>",
                   S.page <= 0 ? " disabled" : "", S.page <= 0 ? " disabled" : "");
        for (int i = first; i < end; i++) {
            char path[256], url[1024];
            style_name(name, sizeof name, i);
            skin_style_preview(item, i, path, sizeof path); // thumbnail: getStylePreview(item, index) (deob-10.0.7.js:108869)
            html_dom_asset_url(path, url, sizeof url);
            sb_appendf(&sb, "<button type='button' class='swatch svelte-143xo7r%s' role='radio' aria-checked='%s' onclick='stylePick(%d)' aria-label='",
                       S.selected == i ? " selected" : "", S.selected == i ? "true" : "false", i);
            sb_append_esc(&sb, name);
            sb_append(&sb, "' title='");
            sb_append_esc(&sb, name);
            sb_append(&sb, "'><img class='swatch-img svelte-143xo7r' alt='' draggable='false' loading='lazy' src='");
            sb_append_esc(&sb, url);
            sb_append(&sb, "'> <span class='swatch-name svelte-143xo7r'>");
            sb_append_esc(&sb, name);
            sb_append(&sb, "</span></button>");
        }
        sb_appendf(&sb, "</div> <button type='button' class='slider-arrow right svelte-143xo7r%s' aria-label='Scroll styles right' onclick='stylePage(1)'%s>"
                        "<span class='slider-arrow-icon svelte-143xo7r'></span></button></div>",
                   end >= item->styles ? " disabled" : "", end >= item->styles ? " disabled" : "");
    }
    if (S.error[0]) {
        sb_append(&sb, " <div class='style-error svelte-143xo7r'>");
        sb_append_esc(&sb, S.error);
        sb_append(&sb, "</div>");
    }
    // Cancel (disabled while applying) and Apply: "Applying…", else "Apply (<name>)" (deob-10.0.7.js:108963-108988)
    sb_appendf(&sb, " <div class='style-actions svelte-143xo7r'><button class='btn btn-cancel svelte-143xo7r' onclick='knViewPop(0)'%s>Cancel</button> ",
               S.busy ? " disabled" : "");
    const int has = S.selected >= 0 && S.selected < item->styles;
    if (has) style_name(name, sizeof name, S.selected);
    sb_appendf(&sb, "<button class='btn btn-apply svelte-143xo7r' onclick='styleApply()'%s>", S.busy || !has ? " disabled" : "");
    if (S.busy) sb_append(&sb, "Applying…");
    else if (has) {
        sb_append(&sb, "Apply (");
        sb_append_esc(&sb, name);
        sb_append(&sb, ")");
    } else sb_append(&sb, "Apply");
    sb_append(&sb, "</button></div></div></div></div></div></div>");
    return sb_finish(&sb);
}

static void redraw(void) {
    if (S.open) v10_view_set(style_html());
}

static void closed(int accepted, void *user) {
    (void) accepted;
    (void) user;
    S.open = 0;
}

static void complete(const cJSON *data, const char *error) {
    S.busy = 0;
    if (S.timer) html_timer_cancel(S.timer);
    S.timer = 0;
    if (!cJSON_IsObject(data)) {
        // error.message || "Failed to apply style" (deob-10.0.7.js:108826-108834)
        snprintf(S.error, sizeof S.error, "%s", error && *error ? error : "Failed to apply style");
        redraw();
        return;
    }
    // The brain persists style in item_styles (db_economy.c:504-535); the local account's
    // copy must also change now, so the loadout and preview render the chosen style.
    const cJSON *applied = cJSON_GetObjectItemCaseSensitive(data, "style");
    const int style = cJSON_IsNumber(applied) ? applied->valueint : S.selected;
    account_style_applied(S.index, S.sid, style);
    Client *client = menu_ui() ? menu_ui()->client : NULL;
    if (client) client_class_preview_invalidate(client);
    if (client && client->net_connected && !client->offline && account_backend() != ACCOUNT_BACKEND_KRUNKER) { // (a KN game server's)
        uint8_t packet[NET_EXT_HEADER_SIZE];
        const uint16_t size = net_ext_pack(packet, sizeof packet, NET_EXT_KN_STYLE_REFRESH, NULL, 0);
        if (size) client_net_send(client, NET_PACKET_EXT, packet, size);
    }
    ui_loadout_owned_changed();
    v10_market_owned_changed();
    // the caller's success toast: "Styled " + item.name + (styleName ? " - " + styleName : "") (deob-10.0.7.js:134262-134263,
    // :147775); styleName is the swatch's name, styles[i] || "Style " + i (:108843, :108867)
    const ViewerSkin *item = viewer_skin(S.index);
    char name[160], text[384];
    style_name(name, sizeof name, style);
    snprintf(text, sizeof text, "Styled %s - %s", item && item->name ? item->name : "", name);
    StrBuf toast = {0};
    sb_append_esc(&toast, text);
    char *html = sb_finish(&toast);
    ui_notify(html);
    free(html);
    if (S.open) v10_view_pop(1);
}

static void on_social(void *user, const cJSON *frame) {
    (void) user;
    if (!S.busy) return;
    const char *label = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 0));
    if (!label) return;
    if (!strcmp(label, "__close")) complete(NULL, NULL);
    else if (!strcmp(label, "istr")) {
        const cJSON *uid = cJSON_GetArrayItem(frame, 2);
        if (cJSON_IsNumber(uid) && uid->valueint == S.uid) complete(cJSON_GetArrayItem(frame, 1), NULL);
    } else if (!strcmp(label, "po")) complete(NULL, cJSON_GetStringValue(cJSON_GetArrayItem(frame, 2)));
}

static void expired(void *user) {
    (void) user;
    S.timer = 0;
    if (S.busy) complete(NULL, NULL);
}

// krunker.io's answer to items.setStyle (deob-10.0.7.js:108823-108845): an error's message, else "Failed to apply
// style"; a success is the picked style (:108840-108844), applied as the brain's answer is.
static void kio_style_answer(void *user, const int status, const cJSON *data, const char *code, const char *message) {
    (void) status;
    (void) data;
    const int style = (int) (intptr_t) user;
    if (message || code) {
        complete(NULL, message && message[0] ? message : NULL);
        return;
    }
    cJSON *done = cJSON_CreateObject();
    cJSON_AddNumberToObject(done, "style", style);
    complete(done, NULL);
    cJSON_Delete(done);
}

static void apply(void) {
    const ViewerSkin *item = viewer_skin(S.index);
    if (!S.open || S.busy || !item || S.selected < 0 || S.selected >= item->styles) return;
    if (account_backend() == ACCOUNT_BACKEND_KRUNKER) {
        // items.setStyle(skinId, {item_id: item.index, style: index}) (deob-10.0.7.js:108823-108826)
        char path[64], body[64];
        snprintf(path, sizeof path, "items/%d/style", S.sid);
        snprintf(body, sizeof body, "{\"item_id\":%d,\"style\":%d}", S.index, S.selected);
        S.busy = 1;
        S.error[0] = 0;
        redraw();
        kio_store_write("POST", path, body, kio_style_answer, (void *) (intptr_t) S.selected); // (a refusal answers at once)
        return;
    }
    cJSON *frame = cJSON_CreateArray();
    cJSON_AddItemToArray(frame, cJSON_CreateString("ist"));
    cJSON_AddItemToArray(frame, cJSON_CreateNumber(S.sid));
    cJSON_AddItemToArray(frame, cJSON_CreateNumber(S.index));
    cJSON_AddItemToArray(frame, cJSON_CreateNumber(S.selected));
    S.uid = ++g_uids;
    cJSON_AddItemToArray(frame, cJSON_CreateNumber(S.uid));
    if (!g_listening) g_listening = backend_social_listen(on_social, NULL);
    backend_social_open();
    const int sent = g_listening && backend_social_send(frame);
    cJSON_Delete(frame);
    if (sent) {
        S.busy = 1;
        S.error[0] = 0;
        S.timer = html_timer_add(STYLE_WAIT_MS, expired, NULL);
    } else snprintf(S.error, sizeof S.error, "Failed to apply style");
    redraw();
}

void style_picker_open(int index, int sid) {
    if (S.busy || !style_picker_can_open(index) || sid <= 0) return;
    S.index = index;
    S.sid = sid;
    S.selected = owned_style(index, sid);
    const ViewerSkin *item = viewer_skin(index);
    if (item && (S.selected < 0 || S.selected >= item->styles)) S.selected = 0;
    S.page = S.selected / STYLE_PAGE_SIZE;
    S.error[0] = 0;
    S.open = 1;
    v10_view_push(style_html(), redraw, closed, NULL);
}

int style_picker_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc) {
    (void) ev;
    if (!name) return -1;
    if (!strcmp(name, "styleOpen")) {
        style_picker_open(picker_arg(args, argc, 0, -1), picker_arg(args, argc, 1, 0));
    } else if (!strcmp(name, "stylePick")) {
        const ViewerSkin *item = viewer_skin(S.index);
        const int pick = picker_arg(args, argc, 0, -1);
        if (S.open && !S.busy && item && pick >= 0 && pick < item->styles) {
            S.selected = pick;
            S.error[0] = 0;
            redraw();
        }
    } else if (!strcmp(name, "stylePage")) {
        if (S.open && !S.busy) {
            const ViewerSkin *item = viewer_skin(S.index);
            const int pages = item ? (item->styles + STYLE_PAGE_SIZE - 1) / STYLE_PAGE_SIZE : 0;
            const int page = S.page + picker_arg(args, argc, 0, 0);
            if (page >= 0 && page < pages) {
                S.page = page;
                redraw();
            }
        }
    } else if (!strcmp(name, "styleApply")) apply();
    else return -1;
    return 1;
}
