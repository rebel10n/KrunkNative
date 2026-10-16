// Krunker v10.0.2's chat-column widgets (client/include/v10_notify.h): the notification center,
// the game announcement banner and the emoji panel, with their markup copied from v10's Svelte
// templates (the svelte-nrigjp / -16a8kon / -1m80xxc / -13g6jqo classes of ui/css/svelte.css) and
// driven over html_dom.h, so the native engine and the web DOM run the same code.
//
// Notification center (g:197557-197640, store g:118762-118787):
//   push(content, {duration = 4000}) appends {id, shownAt, duration} to $notifications; the center
//   renders the list reversed (newest on top), each .notification-item with the slideIn 0.3 s
//   ease-out animation (CSS), its .text-notification (title + text) and, when duration > 0, an
//   expiration bar whose fill is width: (expiresAt - now) / duration * 100 % (transition width
//   0.1 s linear), recomputed on mount and every 100 ms (setInterval 100); at 0 it gets .expired.
//   setTimeout(duration) removes the notification. The center has .hidden while neither the menu
//   ($hasMenuUI: UI.toggleMenu) nor the game HUD ($hasGameUI: toggleGameUI, #uiBase.onGame) is up.
//   Times come from the HUD clock (ui_notify_tick: virtual in --render-scenario), the 100 ms
//   interval is stepped per frame on that clock.
// Game announcement (g:118340-118585, GAME_ANNOUNCEMENT g:210119): .game-announcement gets .show
//   and the .ga-message line while a message is set. Bundles, shop displays, actions and the
//   target filter are not ported: KrunkNative's announcements (server console `announce`) carry
//   a message only.
// Emoji panel (g:197502-197556) and the chat's emoji (g:103080-103125, g:202884-202913,
//   g:221099-221112, g:221347): the emoji map is every store item of type 17 (EMOJI) by its
//   `text` name(s); typing ":<word>" in #chatInput (the text before the caret, emoji already
//   written as ":name:" left out) opens the panel filtered by <word> (usable emoji whose name
//   contains it, sorted by where it occurs), Up / Down move the selection, the chat key or a click
//   writes ":name:" over the ":<word>"; the panel closes 100 ms after #chatInput loses focus.
//   Chat lines show every ":name:" as the emoji's 64 px <img>.
//
// KrunkNative differences (intentional):
//  - text notifications are dismissed by a click on them (v10's only expire; its component ones,
//    Register / Wild Offer / Survey, have their own close button), running the source's action
//    first (a friend request opens the Friends tab). Items carry ids knNotif<N> / knNotifBar<N>,
//    the panel knEmojiPanel / knEmoji<i> (no style; tests and the click handler).
//  - v10 evaluates the emoji query on keyup; here once per frame when #chatInput's value or caret
//    changed (the same result, up to a key's hold time earlier).
//  - the chat key with the panel open and no emoji listed sends the message (v10 throws on
//    filtered[selected][0] and the key does nothing: a line ending in ':' could not be sent).
//  - the sources: v10 pushes only Register, Wild Offer and Survey; KrunkNative pushes its friend
//    requests, direct messages, gifts / trades and system messages (ui_notify.c) as text ones.
#include <v10_notify.h>
#include <html_ui.h>
#include <ui_logic.h>
#include <ui_tooltip.h>
#include <backend_ui.h>
#include <online_mode.h>
#include <net_ext.h>
#include "../viewer/viewer_store.h"
#include <cJSON.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NOTIFY_MAX 16
#define NOTIFY_BAR_MS 100.0   // setInterval(update, 100)
#define EMOJI_MAX 128
#define EMOJI_NAME_MAX 32
#define EMOJI_BLUR_MS 100.0   // chatInput blur: setTimeout(() => $emojiPanelOpen = false, 100)
#define LRM "\xe2\x80\x8e"    // U+200E (UTILS.fixStr)

typedef struct {
    int id;
    char title[128];
    char text[512];
    double shown_at;    // HUD clock ms; < 0 = set on the next tick (pushed before the first frame)
    double duration;    // ms, <= 0 = no bar, no expiry
    double next_bar;    // the next 100 ms refresh of the bar
    double pinned_pct;  // scenario: the bar frozen at this width, no expiry (< 0 = live)
    int expired;        // .expired on the bar
    int mounted;        // the item is in the page
    V10NotifyClick on_click;
    void *user;
    char *html;         // a component notification's markup (v10_notify_push_html), else NULL
} Notif;

typedef struct {
    char name[EMOJI_NAME_MAX]; // the map key ("love")
    int skin;                  // store index
} Emoji;

static struct {
    Notif items[NOTIFY_MAX];
    int count;
    int next_id;
    double now;            // last HUD clock ms (ui_notify_tick), < 0 before the first frame
    unsigned page_serial;  // the page the items are mounted in
    int hidden;            // the center's .hidden as written (-1 unknown)
    int handlers;          // knNotifyClick / selectEmoji added
    // game announcement
    char announce[NET_GAME_ANNOUNCEMENT_TEXT];
    int announce_shown;    // as written into the page (-1 unknown)
    // emoji
    Emoji map[EMOJI_MAX];
    int map_count, map_built;
    int list[EMOJI_MAX];   // $emojiPanelFiltered: indices into map
    int list_count;
    int open;              // $emojiPanelOpen
    int mounted;           // the panel's markup is in #emojiPanelHolder
    int selected;          // $emojiSelected
    char query[64];        // $emojiQuery
    char last_value[256];  // #chatInput as last evaluated
    int last_caret;
    int focused;           // #chatInput had focus last frame
    double blur_at;        // the blur's 100 ms close, < 0 none
} N = {.now = -1.0, .hidden = -1, .announce_shown = -1, .blur_at = -1.0};

// ------------------------------------------------------------------ helpers

static void esc_into(StrBuf *sb, const char *s) { sb_append_esc(sb, s ? s : ""); }

static HtmlElem center_el(void) { return html_dom_query(0, "#notificationCenter > .notification-center"); }

static void notif_ids(const Notif *n, char *item, size_t ni, char *bar, size_t nb) {
    if (item) snprintf(item, ni, "knNotif%d", n->id);
    if (bar) snprintf(bar, nb, "knNotifBar%d", n->id);
}

static double bar_pct(const Notif *n) {
    if (n->pinned_pct >= 0.0) return n->pinned_pct;
    if (n->shown_at < 0.0 || N.now < 0.0) return 100.0; // the initial $state(100)
    const double pct = (n->shown_at + n->duration - N.now) / n->duration * 100.0; // Math.max(0, left / duration * 100)
    return pct > 0.0 ? pct : 0.0;
}

// ------------------------------------------------------------------ notification center

static void page_check(void) {
    const unsigned serial = html_ui_page_serial();
    if (serial == N.page_serial) return;
    N.page_serial = serial;
    for (int i = 0; i < N.count; i++) N.items[i].mounted = 0; // a new page: mount again
    N.hidden = -1;
    N.announce_shown = -1;
    N.mounted = 0;
    N.open = 0;
    N.focused = 0;
    N.last_value[0] = 0;
    N.last_caret = -1;
    N.blur_at = -1.0;
}

static void item_html(StrBuf *sb, const Notif *n) {
    char item[32], bar[32];
    notif_ids(n, item, sizeof(item), bar, sizeof(bar));
    // iîíííïï "<div class=notification-item><!> <!></div>" with iîiîiíì (text) and, duration > 0,
    // iíîïíiï (the expiration bar); onclick / id: KrunkNative
    if (n->html) { // a component (iíiìïïi({content}): Wild Offer, ...): its own controls, no click to dismiss
        sb_appendf(sb, "<div class=\"notification-item svelte-nrigjp\" id=\"%s\">", item);
        sb_append(sb, n->html);
        sb_append(sb, " ");
    } else {
        sb_appendf(sb, "<div class=\"notification-item svelte-nrigjp\" id=\"%s\" onclick=\"knNotifyClick(%d)\">", item, n->id);
        sb_append(sb, "<div class=\"text-notification svelte-nrigjp\"><h3 class=\"notification-title svelte-nrigjp\">");
        esc_into(sb, n->title);
        sb_append(sb, "</h3> <p class=\"notification-text svelte-nrigjp\">");
        esc_into(sb, n->text);
        sb_append(sb, "</p></div> ");
    }
    if (n->duration > 0.0) {
        sb_appendf(sb, "<div class=\"expiration-bar svelte-16a8kon%s\"><div class=\"expiration-bar-track svelte-16a8kon\">"
                       "<div class=\"expiration-bar-fill svelte-16a8kon\" id=\"%s\" style=\"width: %.17g%%\"></div></div></div>",
                   n->expired ? " expired" : "", bar, bar_pct(n));
    }
    sb_append(sb, "</div>");
}

static void mount_items(void) {
    const HtmlElem center = center_el();
    if (!center) return;
    // oldest first, each inserted at the top: the newest ends up first ([...list].reverse())
    for (int i = 0; i < N.count; i++) {
        Notif *n = &N.items[i];
        if (n->mounted) continue;
        StrBuf sb = {0};
        item_html(&sb, n);
        char *html = sb_finish(&sb);
        html_dom_insert_html(center, HTML_INSERT_AFTER_BEGIN, html);
        free(html);
        n->mounted = 1;
        printf("[notify] show #%d \"%s\": %s (%.0f ms)\n", n->id, n->title, n->text, n->duration);
    }
}

static void unmount(const Notif *n) {
    if (!n->mounted) return;
    char item[32];
    notif_ids(n, item, sizeof(item), NULL, 0);
    const HtmlElem el = html_dom_by_id(item);
    if (el) html_dom_remove(el);
}

static int find(const int id) {
    for (int i = 0; i < N.count; i++)
        if (N.items[i].id == id) return i;
    return -1;
}

static void remove_at(const int i, const char *why) {
    if (i < 0 || i >= N.count) return;
    printf("[notify] %s #%d\n", why, N.items[i].id);
    unmount(&N.items[i]);
    free(N.items[i].html);
    memmove(&N.items[i], &N.items[i + 1], (size_t) (N.count - i - 1) * sizeof(N.items[0]));
    N.count--;
}

int v10_notify_push_ex(const char *title, const char *content, const double duration_ms, const V10NotifyClick on_click, void *user) {
    if (N.count == NOTIFY_MAX) remove_at(0, "dropped"); // the oldest goes (v10's list is unbounded)
    Notif *n = &N.items[N.count++];
    memset(n, 0, sizeof(*n));
    n->id = ++N.next_id;
    snprintf(n->title, sizeof(n->title), "%s", title ? title : "");
    snprintf(n->text, sizeof(n->text), "%s", content ? content : "");
    n->duration = duration_ms > 0.0 ? duration_ms : 0.0;
    n->shown_at = N.now >= 0.0 ? N.now : -1.0; // Date.now()
    n->next_bar = n->shown_at + NOTIFY_BAR_MS;
    n->pinned_pct = -1.0;
    n->on_click = on_click;
    n->user = user;
    if (html_ui_available() && html_ui_is_ready()) {
        page_check();
        mount_items();
    }
    return n->id;
}

int v10_notify_push_html(const char *title, const char *html, const double duration_ms) {
    const int id = v10_notify_push_ex(title, "", duration_ms, NULL, NULL);
    const int i = find(id);
    if (i < 0) return id;
    Notif *n = &N.items[i];
    if (n->mounted) { // mounted as text already (the page was up): again with the markup
        unmount(n);
        n->mounted = 0;
    }
    n->html = strdup(html ? html : "");
    if (html_ui_available() && html_ui_is_ready()) mount_items();
    return id;
}

int v10_notify_push(const char *title, const char *content, const double duration_ms) {
    return v10_notify_push_ex(title, content, duration_ms, NULL, NULL);
}

void v10_notify_dismiss(const int id) { remove_at(find(id), "dismiss"); }

void v10_notify_clear(void) {
    while (N.count) remove_at(N.count - 1, "clear");
}

int v10_notify_count(void) { return N.count; }

static int center_hidden(void) {
    // !$hasMenuUI && !$hasGameUI
    const MenuUi *menu = menu_ui();
    const HtmlElem base = html_dom_by_id("uiBase");
    const int game_ui = base && html_dom_has_class(base, "onGame");
    return !(menu && menu->menu_open) && !game_ui;
}

int v10_notify_visible(void) {
    if (!html_ui_available() || !html_ui_is_ready()) return 0;
    const HtmlElem holder = html_dom_by_id("chatHolder");
    return holder && center_el() && html_dom_is_shown(holder) && !center_hidden();
}

static void notify_click(const int id) {
    const int i = find(id);
    if (i < 0) return;
    const V10NotifyClick cb = N.items[i].on_click;
    void *user = N.items[i].user;
    printf("[notify] click #%d\n", id);
    if (cb) cb(user);
    v10_notify_dismiss(id); // the callback may have dismissed it already: unknown ids are ignored
}

static void tick_center(void) {
    const int hidden = center_hidden();
    if (hidden != N.hidden) {
        const HtmlElem center = center_el();
        if (center) {
            html_dom_set_class(center, "hidden", hidden);
            N.hidden = hidden;
        }
    }
    mount_items();
    for (int i = 0; i < N.count; i++) {
        Notif *n = &N.items[i];
        if (n->shown_at < 0.0) { // pushed before the first frame: its clock starts now
            n->shown_at = N.now;
            n->next_bar = N.now + NOTIFY_BAR_MS;
        }
        if (n->duration <= 0.0 || n->pinned_pct >= 0.0) continue;
        if (N.now >= n->shown_at + n->duration) { // setTimeout(() => dismiss(id), duration)
            remove_at(i--, "expired");
            continue;
        }
        if (N.now < n->next_bar) continue;
        while (n->next_bar <= N.now) n->next_bar += NOTIFY_BAR_MS;
        char bar[32], style[48];
        notif_ids(n, NULL, 0, bar, sizeof(bar));
        const HtmlElem fill = html_dom_by_id(bar);
        if (!fill) continue;
        const double pct = bar_pct(n);
        if (pct <= 0.0 && !n->expired) {
            n->expired = 1;
            const HtmlElem track_bar = html_dom_parent(html_dom_parent(fill));
            if (track_bar) html_dom_set_class(track_bar, "expired", 1);
        }
        snprintf(style, sizeof(style), "%.17g%%", pct);
        html_dom_set_style(fill, "width", style);
    }
}

// ------------------------------------------------------------------ game announcement

void v10_announce_set(const char *message) {
    snprintf(N.announce, sizeof(N.announce), "%s", message ? message : "");
    N.announce_shown = -1;
    printf("[notify] announcement %s%s%s\n", N.announce[0] ? "\"" : "cleared", N.announce, N.announce[0] ? "\"" : "");
}

void v10_announce_on_packet(const uint8_t *body, const uint16_t length) {
    NetGameAnnouncementPacket p;
    memset(&p, 0, sizeof(p));
    memcpy(&p, body, length < sizeof(p) ? length : sizeof(p));
    p.message[sizeof(p.message) - 1] = 0;
    v10_announce_set(p.message);
}

static void tick_announcement(void) {
    const int show = N.announce[0] != 0;
    if (show == N.announce_shown) return;
    const HtmlElem el = html_dom_query(0, "#gameAnnouncementEl > .game-announcement");
    if (!el) return;
    // iiìîïíi "<div class=game-announcement><!> <!> <!> <!></div>": the message line, then the
    // (not ported) bundle, products and actions anchors
    StrBuf sb = {0};
    if (show) {
        sb_append(&sb, "<div class=\"ga-message svelte-1m80xxc\">");
        esc_into(&sb, N.announce);
        sb_append(&sb, "</div>");
    }
    sb_append(&sb, "   ");
    char *html = sb_finish(&sb);
    html_dom_set_html(el, html);
    free(html);
    html_dom_set_class(el, "show", show);
    N.announce_shown = show;
}

// ------------------------------------------------------------------ emoji

const cJSON *ui_loadout_skin_json(int index); // ui_loadout.c: assets/skins.json's entry

static void map_add(const char *name, const int skin) {
    if (!name || !*name || strlen(name) >= EMOJI_NAME_MAX) return;
    for (int i = 0; i < N.map_count; i++) {
        if (!strcmp(N.map[i].name, name)) { // emojiMap[text] = index: the key keeps its place
            N.map[i].skin = skin;
            return;
        }
    }
    if (N.map_count == EMOJI_MAX) return;
    snprintf(N.map[N.map_count].name, EMOJI_NAME_MAX, "%s", name);
    N.map[N.map_count++].skin = skin;
}

static void map_build(void) {
    if (N.map_built) return;
    N.map_built = 1;
    for (int i = 0; i < g_viewer_skin_count; i++) {
        const ViewerSkin *s = viewer_skin(i);
        if (!s || s->type != 17) continue; // EMOJI
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(ui_loadout_skin_json(i), "text"), *t;
        if (cJSON_IsString(text)) map_add(text->valuestring, i);
        else cJSON_ArrayForEach(t, text) if (cJSON_IsString(t)) map_add(t->valuestring, i); // a list of names
    }
}

static int map_find(const char *name, const size_t len) {
    if (!len || len >= EMOJI_NAME_MAX) return -1;
    for (int i = 0; i < N.map_count; i++)
        if (strlen(N.map[i].name) == len && !memcmp(N.map[i].name, name, len)) return i;
    return -1;
}

static int account_skin_count(const int index) {
    const AccountInfo *acc = active_account();
    const cJSON *inventory = acc && acc->data ? cJSON_GetArrayItem(acc->data, 10) : NULL, *e;
    cJSON_ArrayForEach(e, inventory) {
        const cJSON *ind = cJSON_GetObjectItem(e, "ind"), *cnt = cJSON_GetObjectItem(e, "cnt");
        if (cJSON_IsNumber(ind) && ind->valueint == index) return cJSON_IsNumber(cnt) ? cnt->valueint : 1;
    }
    return 0;
}

// iiíìiîì(skin, account, ..., type EMOJI): free, a dev account, owned. Offline every item is
// unlocked (ui_loadout.c all_skins_unlocked: the sandbox has no inventory).
static int emoji_usable(const int skin) {
    const ViewerSkin *s = viewer_skin(skin);
    if (!s) return 0;
    if (!online_mode_online() || (s->flags & VS_FREE)) return 1;
    const AccountInfo *acc = active_account();
    return acc && (acc->is_dev || account_skin_count(skin) > 0);
}

static int ci_find(const char *hay, const char *needle) { // toLowerCase().indexOf
    const size_t hn = strlen(hay), nn = strlen(needle);
    for (size_t i = 0; i + nn <= hn; i++) {
        size_t k = 0;
        while (k < nn && tolower((unsigned char) hay[i + k]) == tolower((unsigned char) needle[k])) k++;
        if (k == nn) return (int) i;
    }
    return -1;
}

static void filter_list(void) { // iíîîïiì(emojiMap, query, skins, account)
    map_build();
    N.list_count = 0;
    int pos[EMOJI_MAX];
    for (int i = 0; i < N.map_count; i++) {
        if (!emoji_usable(N.map[i].skin)) continue;
        const int at = ci_find(N.map[i].name, N.query);
        if (at < 0) continue;
        int k = N.list_count++; // stable insertion by indexOf (Array.prototype.sort is stable)
        while (k > 0 && pos[k - 1] > at) {
            N.list[k] = N.list[k - 1];
            pos[k] = pos[k - 1];
            k--;
        }
        N.list[k] = i;
        pos[k] = at;
    }
}

static void item_markup(StrBuf *sb, const int k) {
    const Emoji *e = &N.map[N.list[k]];
    char thumb[256];
    store_skin_preview(e->skin, thumb, sizeof(thumb));
    sb_appendf(sb, "<div class=\"emoji-item svelte-13g6jqo%s\" role=\"button\" tabindex=\"0\" id=\"knEmoji%d\" onclick=\"selectEmoji(%d)\">"
                   "<div class=\"emoji-thumbnail svelte-13g6jqo\"><img class=\"svelte-13g6jqo\" src=\"",
               k == N.selected ? " selected" : "", k, k);
    esc_into(sb, thumb);
    sb_append(sb, "\" alt=\"");
    esc_into(sb, e->name);
    sb_append(sb, "\"></div> <div class=\"emoji-label svelte-13g6jqo\">");
    esc_into(sb, e->name);
    sb_append(sb, "</div></div>");
}

static void list_markup(StrBuf *sb) {
    for (int k = 0; k < N.list_count; k++) item_markup(sb, k);
}

static void scroll_selected(void) { // scrollIntoView({block: "nearest"}) of .emoji-item.selected
    char id[24];
    snprintf(id, sizeof(id), "knEmoji%d", N.selected);
    const HtmlElem item = html_dom_by_id(id), content = html_dom_query(0, "#knEmojiPanel .emoji-content");
    HtmlRect ir, cr;
    HtmlScroll sc;
    if (!item || !content || !html_dom_get_rect(item, &ir) || !html_dom_get_rect(content, &cr) || !html_dom_get_scroll(content, &sc)) return;
    float top = sc.top;
    const float view_h = sc.client_h > 0.0f ? sc.client_h : cr.h;
    if (ir.y < cr.y) top -= cr.y - ir.y;
    else if (ir.y + ir.h > cr.y + view_h) top += ir.y + ir.h - (cr.y + view_h);
    if (top != sc.top) html_dom_set_scroll(content, sc.left, top);
}

static void panel_render(void) {
    const HtmlElem holder = html_dom_by_id("emojiPanelHolder");
    if (!holder) return;
    if (!N.open) {
        if (N.mounted) html_dom_set_html(holder, "");
        N.mounted = 0;
        return;
    }
    StrBuf sb = {0};
    if (!N.mounted) { // {#if $emojiPanelOpen}: mounted once per opening (slideInUp plays then)
        sb_append(&sb, "<div class=\"emoji-panel io-popup-container svelte-13g6jqo\" id=\"knEmojiPanel\"><div class=\"emoji-header io-popup-header "
                       "io-popup-header--slim svelte-13g6jqo\"><i class=\"material-icons emoji-icon svelte-13g6jqo\">emoji_emotions</i> Emoji Collection ");
        sb_appendf(&sb, "<div class=\"emoji-count svelte-13g6jqo\">%d emoji</div></div> <div class=\"emoji-content io-popup-content svelte-13g6jqo\">"
                        "<div class=\"emoji-list svelte-13g6jqo\">", N.list_count);
        list_markup(&sb);
        sb_append(&sb, "</div></div></div>");
        char *html = sb_finish(&sb);
        html_dom_set_html(holder, html);
        free(html);
        N.mounted = 1;
        printf("[emoji] panel open \"%s\": %d emoji\n", N.query, N.list_count);
    } else { // the keyed each block: the list and the count follow the filter
        const HtmlElem list = html_dom_query(holder, ".emoji-list"), count = html_dom_query(holder, ".emoji-count");
        list_markup(&sb);
        char *html = sb_finish(&sb);
        if (list) html_dom_set_html(list, html);
        free(html);
        char text[32];
        snprintf(text, sizeof(text), "%d emoji", N.list_count);
        if (count) html_dom_set_text(count, text);
    }
    scroll_selected();
}

static void set_selected(const int k) {
    if (k == N.selected) return;
    char id[24];
    snprintf(id, sizeof(id), "knEmoji%d", N.selected);
    HtmlElem el = html_dom_by_id(id);
    if (el) html_dom_set_class(el, "selected", 0);
    N.selected = k;
    snprintf(id, sizeof(id), "knEmoji%d", N.selected);
    if ((el = html_dom_by_id(id))) html_dom_set_class(el, "selected", 1);
    scroll_selected();
}

static void panel_close(void) {
    if (!N.open) return;
    N.open = 0;
    panel_render();
}

// The keyup evaluation (g:202898-202913) of #chatInput's text before the caret.
static void evaluate(const char *value, const int caret) {
    char before[256];
    const int n = caret < 0 ? (int) strlen(value) : caret;
    snprintf(before, sizeof(before), "%.*s", n, value);
    // .replace(/:([a-zA-Z0-9_]+):/g, (m, name) => emojiMap[name] === undefined ? m : "")
    char kept[256];
    size_t o = 0;
    for (const char *p = before; *p && o + 1 < sizeof(kept);) {
        if (*p == ':') {
            const char *q = p + 1;
            while (isalnum((unsigned char) *q) || *q == '_') q++;
            if (q > p + 1 && *q == ':' && map_find(p + 1, (size_t) (q - p - 1)) >= 0) {
                p = q + 1;
                continue;
            }
        }
        kept[o++] = *p++;
    }
    kept[o] = 0;
    const char *colon = strrchr(kept, ':');
    int open = 0;
    char query[64] = "";
    if (colon) {
        const char *w = colon + 1;
        int word = 1;
        for (const char *q = w; *q; q++) word &= isalnum((unsigned char) *q) || *q == '_';
        if (word && strlen(w) < sizeof(query)) {
            open = 1;
            snprintf(query, sizeof(query), "%s", w);
        }
    }
    const int query_changed = strcmp(query, N.query) != 0;
    snprintf(N.query, sizeof(N.query), "%s", query);
    if (query_changed) N.selected = 0;
    const int was_open = N.open;
    N.open = open;
    if (!open) {
        if (was_open) panel_render();
        return;
    }
    if (!was_open || query_changed) {
        filter_list();
        panel_render();
    }
}

static HtmlElem chat_input(void) { return html_dom_by_id("chatInput"); }

static void select_emoji(const int k) { // window.selectEmoji(index)
    const HtmlElem input = chat_input();
    if (!input) return;
    char value[256] = "";
    html_dom_get_value(input, value, sizeof(value));
    int start = 0, end = 0;
    if (!html_dom_text_selection(input, &start, &end)) start = (int) strlen(value);
    char before[256];
    snprintf(before, sizeof(before), "%.*s", start, value);
    const char *colon = strrchr(before, ':');
    if (colon && k >= 0 && k < N.list_count) {
        const int at = (int) (colon - before);
        char insert[EMOJI_NAME_MAX + 4];
        snprintf(insert, sizeof(insert), ":%s:", N.map[N.list[k]].name);
        html_dom_text_replace(input, at, start, insert); // value = head + ":" + name + ":" + tail
        html_dom_text_set_selection(input, at + (int) strlen(insert), at + (int) strlen(insert));
        printf("[emoji] selected :%s:\n", N.map[N.list[k]].name);
    }
    if (html_dom_active_element() != input) html_dom_focus(input);
    // $emojiPanelOpen = false; the next keyup re-evaluates (here: the next changed frame)
    N.open = 0;
    panel_render();
    html_dom_get_value(input, N.last_value, sizeof(N.last_value));
    if (!html_dom_text_selection(input, &N.last_caret, &end)) N.last_caret = -1;
}

int v10_emoji_chat_key(void) {
    if (!N.open || html_dom_active_element() != chat_input()) return 0;
    if (!N.list_count) return 0; // KrunkNative: nothing to pick, the key sends (v10 throws)
    select_emoji(N.selected);
    return 1;
}

int v10_emoji_arrow_key(const int code) {
    if ((code != 38 && code != 40) || !N.open || html_dom_active_element() != chat_input()) return 0;
    if (N.list_count) set_selected((N.selected + N.list_count + (code == 38 ? -1 : 1)) % N.list_count);
    return 1; // preventDefault
}

static void tick_emoji(void) {
    const HtmlElem input = chat_input();
    const int focused = input && html_dom_active_element() == input;
    if (focused) {
        N.blur_at = -1.0;
        char value[256] = "";
        int caret = -1, end = 0;
        html_dom_get_value(input, value, sizeof(value));
        if (!html_dom_text_selection(input, &caret, &end)) caret = -1;
        if (!N.focused || caret != N.last_caret || strcmp(value, N.last_value)) {
            snprintf(N.last_value, sizeof(N.last_value), "%s", value);
            N.last_caret = caret;
            evaluate(value, caret);
        }
    } else if (N.focused) {
        N.blur_at = N.now + EMOJI_BLUR_MS; // chatInput blur
    }
    N.focused = focused;
    if (N.blur_at >= 0.0 && N.now >= N.blur_at) {
        N.blur_at = -1.0;
        N.last_value[0] = 0;
        N.last_caret = -1;
        panel_close();
    }
}

void v10_emoji_append_chat(char *buf, const size_t size, size_t *len, const char *msg) {
    if (!buf || !size || !len || *len >= size) return;
    map_build();
    StrBuf sb = {0};
    sb_append(&sb, LRM);
    for (const char *p = msg ? msg : ""; *p;) {
        if (*p == ':') {
            const char *q = p + 1;
            while (isalnum((unsigned char) *q) || *q == '_') q++;
            const int e = q > p + 1 && *q == ':' ? map_find(p + 1, (size_t) (q - p - 1)) : -1;
            const ViewerSkin *s = e >= 0 ? viewer_skin(N.map[e].skin) : NULL;
            if (s) {
                char thumb[256];
                store_skin_preview(N.map[e].skin, thumb, sizeof(thumb));
                StrBuf img = {0};
                sb_append(&img, "<img src=\"");
                esc_into(&img, thumb);
                sb_append(&img, "\" width=\"64\" height=\"64\" style=\"vertical-align:middle; margin: -5px 0;\" alt=\"");
                esc_into(&img, s->name);
                sb_append(&img, "\">");
                // room for it and the row's closing "</span></div><br>" + U+200E; else it stays text
                if (*len + sb.len + img.len + 48 < size) {
                    sb_append_n(&sb, img.data, img.len);
                    printf("[emoji] chat :%s:\n", N.map[e].name);
                    p = q + 1;
                    sb_free(&img);
                    continue;
                }
                sb_free(&img);
            }
        }
        const char c[2] = {*p++, 0};
        sb_append_esc(&sb, c);
    }
    sb_append(&sb, LRM);
    char *out = sb_finish(&sb);
    const size_t n = strlen(out);
    if (*len + n < size) {
        memcpy(buf + *len, out, n + 1);
        *len += n;
    } else { // truncated at a character boundary outside markup is not guaranteed: drop the tail
        size_t keep = size - 1 - *len;
        while (keep > 0 && (out[keep] & 0xC0) == 0x80) keep--;
        const char *lt = memchr(out, '<', keep);
        if (lt) keep = (size_t) (lt - out); // never cut an <img> in half
        memcpy(buf + *len, out, keep);
        *len += keep;
        buf[*len] = 0;
    }
    free(out);
}

// ------------------------------------------------------------------ handlers, frame, scenario

static int h_notify_click(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user;
    (void) ev;
    if (argc > 0 && args[0].kind == HTML_ARG_NUM) notify_click((int) args[0].num);
    return 1;
}

static int h_select_emoji(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user;
    (void) ev;
    select_emoji(argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : N.selected);
    return 1;
}

int v10_notify_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    if (!name) return -1;
    if (!strcmp(name, "knNotifyClick")) return h_notify_click(NULL, ev, args, argc);
    if (!strcmp(name, "selectEmoji")) return h_select_emoji(NULL, ev, args, argc);
    return -1;
}

void v10_notify_tick(const double now_ms) {
    N.now = now_ms;
    if (!html_ui_available() || !html_ui_is_ready()) return;
    if (!N.handlers) {
        N.handlers = 1;
        html_ui_add_handler("knNotifyClick", h_notify_click);
        html_ui_add_handler("selectEmoji", h_select_emoji);
    }
    page_check();
    tick_center();
    tick_announcement();
    tick_emoji();
}

static double jnum(const cJSON *o, const char *k, const double def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

static const char *jstr(const cJSON *o, const char *k) {
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, k));
    return s ? s : "";
}

void v10_notify_scenario(const cJSON *state) {
    if (!cJSON_IsObject(state)) return;
    if (!N.handlers) {
        N.handlers = 1;
        html_ui_add_handler("knNotifyClick", h_notify_click);
        html_ui_add_handler("selectEmoji", h_select_emoji);
    }
    page_check();
    const cJSON *n;
    cJSON_ArrayForEach(n, cJSON_GetObjectItemCaseSensitive(state, "notifications")) { // oldest first, like pushes
        const int id = v10_notify_push(jstr(n, "title"), jstr(n, "text"), jnum(n, "duration", V10_NOTIFY_DEFAULT_MS));
        const int i = find(id);
        const double pct = jnum(n, "remainingPct", -1.0);
        if (i >= 0 && pct >= 0.0 && N.items[i].duration > 0.0) { // a frozen bar (the reference's width)
            N.items[i].pinned_pct = pct;
            char bar[32], style[48];
            notif_ids(&N.items[i], NULL, 0, bar, sizeof(bar));
            snprintf(style, sizeof(style), "%.17g%%", pct);
            const HtmlElem fill = html_dom_by_id(bar);
            if (fill) html_dom_set_style(fill, "width", style);
        }
    }
    const cJSON *ann = cJSON_GetObjectItemCaseSensitive(state, "announcement");
    if (cJSON_IsString(ann)) {
        v10_announce_set(ann->valuestring);
        tick_announcement();
    }
    const cJSON *emoji = cJSON_GetObjectItemCaseSensitive(state, "emoji");
    if (cJSON_IsObject(emoji)) { // {chat: "hi :lo", selected: k}: #chatInput focused with the text, the caret at its end
        const HtmlElem input = chat_input();
        if (input) {
            const char *text = jstr(emoji, "chat");
            html_dom_set_value(input, text);
            html_dom_focus(input);
            html_dom_text_set_selection(input, (int) strlen(text), (int) strlen(text));
            snprintf(N.last_value, sizeof(N.last_value), "%s", text);
            N.last_caret = (int) strlen(text);
            N.focused = 1;
            evaluate(text, N.last_caret);
            const int sel = (int) jnum(emoji, "selected", 0);
            for (int k = 0; k < sel && N.open; k++) v10_emoji_arrow_key(40);
        }
    }
    const cJSON *tip = cJSON_GetObjectItemCaseSensitive(state, "tip");
    if (cJSON_IsObject(tip)) { // {selector, kind: "item" | "streak" | "spin"}: the hovered element's tooltip
        const HtmlElem el = html_dom_query(0, jstr(tip, "selector"));
        const char *kind = jstr(tip, "kind");
        if (!el) fprintf(stderr, "[scenario] notify.tip: no %s\n", jstr(tip, "selector"));
        else if (!strcmp(kind, "streak")) ui_tooltip_streak(el);
        else if (!strcmp(kind, "spin")) ui_tooltip_spin_history(el);
        else ui_tooltip_item(el);
    }
}
