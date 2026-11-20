// v10.0.7 owned-item style picker. Selection belongs to a stack or a specific painted copy,
// and is written through the brain's items.setStyle frame (deob-10.0.7.js:27102-27106,
// :108817-108850). The response triggers an account refresh, which carries m.style.
#include <style_picker.h>
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Market views reuse the refreshed inventory after the account's owned style changes.
void v10_market_owned_changed(void);

#define STYLE_WAIT_MS 15000
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

static int owned_style(int index, int sid) {
    const cJSON *owned = owned_list(), *row;
    cJSON_ArrayForEach(row, owned) {
        const cJSON *ind = cJSON_GetObjectItemCaseSensitive(row, "ind");
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(row, "id");
        if (!cJSON_IsNumber(ind) || ind->valueint != index) continue;
        if (sid >= COPY_ID_BASE ? !cJSON_IsNumber(id) || id->valueint != sid : cJSON_IsNumber(id)) continue;
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
    return item && item->styles > 1 && account_backend() != ACCOUNT_BACKEND_KRUNKER && active_account();
}

static char *style_html(void) {
    const ViewerSkin *item = viewer_skin(S.index);
    StrBuf sb = {0};
    if (!item) return sb_finish(&sb);
    sb_append(&sb, "<div class='popup-container svelte-1wz7ymp'><div class='popup svelte-1wz7ymp' style='width:700px'>"
                   "<div class='style-container io-popup-container svelte-143xo7r'><div class='style-content io-popup-content svelte-143xo7r' style='height:auto;min-height:330px'>"
                   "<div class='style-header svelte-143xo7r'><div class='item-name svelte-143xo7r'>");
    sb_append_esc(&sb, item->name ? item->name : "Item Styles");
    sb_append(&sb, "</div><div class='item-sub svelte-143xo7r'>Pick a style to preview, then press Apply.</div></div>"
                   "<div class='style-controls svelte-143xo7r'><div class='swatch-slider svelte-143xo7r'>");
    sb_appendf(&sb, "<button type='button' class='slider-arrow left svelte-143xo7r' aria-label='Previous styles' onclick='stylePage(-1)'%s>"
                    "<span class='slider-arrow-icon svelte-143xo7r'></span></button>", S.page <= 0 ? " disabled" : "");
    sb_append(&sb, "<div class='swatch-row svelte-143xo7r' role='radiogroup' aria-label='Item styles'>");
    const cJSON *raw = ui_loadout_skin_json(S.index);
    const cJSON *styles = cJSON_GetObjectItemCaseSensitive(raw, "styles");
    char base[512];
    viewer_skin_preview_path(item, base, sizeof base);
    const int end = (S.page + 1) * STYLE_PAGE_SIZE < item->styles ? (S.page + 1) * STYLE_PAGE_SIZE : item->styles;
    for (int i = S.page * STYLE_PAGE_SIZE; i < end; i++) {
        const char *name = cJSON_GetStringValue(cJSON_GetArrayItem(styles, i));
        char path[600], url[1024];
        skin_style_picker_preview_path(path, sizeof path, base, i, item->styles, item->style_ext);
        // These newly recovered v10 previews live under assets/, not ui/textures/.
        html_dom_asset_url(path, url, sizeof url);
        sb_appendf(&sb, "<button type='button' class='swatch%s svelte-143xo7r' role='radio' aria-checked='%s'"
                        " onclick='stylePick(%d)' style='min-width:90px'>"
                        "<img class='swatch-img svelte-143xo7r' loading='lazy' src='%s' alt=''>"
                        "<span class='swatch-name svelte-143xo7r'>",
                   S.selected == i ? " selected" : "", S.selected == i ? "true" : "false", i, url);
        sb_append_esc(&sb, name ? name : i == 0 ? "Default" : "Style");
        sb_append(&sb, "</span></button>");
    }
    sb_append(&sb, "</div>");
    sb_appendf(&sb, "<button type='button' class='slider-arrow right svelte-143xo7r' aria-label='Next styles' onclick='stylePage(1)'%s>"
                    "<span class='slider-arrow-icon svelte-143xo7r'></span></button>",
               end >= item->styles ? " disabled" : "");
    sb_append(&sb, "</div>");
    if (S.error[0]) {
        sb_append(&sb, "<div class='style-error svelte-143xo7r'>");
        sb_append_esc(&sb, S.error);
        sb_append(&sb, "</div>");
    }
    sb_append(&sb, "<div class='style-actions svelte-143xo7r'><button class='btn btn-cancel svelte-143xo7r' onclick='knViewPop(0)'>Cancel</button>");
    sb_appendf(&sb, "<button class='btn btn-apply svelte-143xo7r' onclick='styleApply()'%s>%s</button>",
               S.busy ? " disabled" : "", S.busy ? "Saving..." : "Apply Style");
    sb_append(&sb, "</div></div></div></div></div>");
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
        snprintf(S.error, sizeof S.error, "%s", error && *error ? error : "Could not save this style.");
        redraw();
        return;
    }
    // The brain persists style in item_styles (db_economy.c:504-535); the local account's
    // copy must also change now, so the loadout and preview render the chosen style.
    account_style_applied(S.index, S.sid, S.selected);
    Client *client = menu_ui() ? menu_ui()->client : NULL;
    if (client) client_class_preview_invalidate(client);
    if (client && client->net_connected && !client->offline) {
        uint8_t packet[NET_EXT_HEADER_SIZE];
        const uint16_t size = net_ext_pack(packet, sizeof packet, NET_EXT_KN_STYLE_REFRESH, NULL, 0);
        if (size) client_net_send(client, NET_PACKET_EXT, packet, size);
    }
    ui_loadout_owned_changed();
    v10_market_owned_changed();
    ui_notify("Item style saved.");
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

static void apply(void) {
    const ViewerSkin *item = viewer_skin(S.index);
    if (!S.open || S.busy || !item || S.selected < 0 || S.selected >= item->styles) return;
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
    } else snprintf(S.error, sizeof S.error, "Could not send style selection.");
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
        if (S.open && !S.busy) {
            S.selected = picker_arg(args, argc, 0, -1);
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
