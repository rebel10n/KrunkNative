// Map editor: window.prompt() / window.confirm() stand-ins. Krunker's editor asks for text
// with the browser's modal dialogs (Colorize > Input, Grouping > Color / Custom, Tools >
// Text Generator, Prefab > Import > JSON, the Quick GUI's JSON entries, ...). There is no
// such dialog natively, so both platforms show the same small in-page dialog (#knPrompt,
// styled like Chromium's in ui/css/editor-kn.css) and call back asynchronously: OK passes
// the field's text (confirm: "1"), Cancel / Escape passes NULL. One dialog at a time; a new
// request cancels the open one. Also Krunker's own popups (genericPopup / clearPops: teleport,
// Import Project) in editor.html's #popupHolder.
#include "editor_internal.h"
#include <html_ui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static EdPromptFn g_cb;
static void *g_user;
static int g_confirm;
static EdChoiceFn g_choice_cb; // the open dialog is an ed_choice
static void *g_choice_user;

// ed_choice requests waiting for the open dialog (permission requests, close prompts)
typedef struct {
    char *title, *message;
    char *buttons[4];
    int count;
    EdChoiceFn cb;
    void *user;
} PendingChoice;
#define MAX_PENDING 16
static PendingChoice g_pending[MAX_PENDING];
static int g_pending_count;

static void show_choice(const char *title, const char *message, const char *const *buttons, int count, EdChoiceFn cb, void *user);

int ed_prompt_open(void) { return g_cb != NULL || g_choice_cb != NULL || html_dom_by_id("knPrompt") != 0; }

static void next_pending(void) {
    if (!g_pending_count || ed_prompt_open()) return;
    PendingChoice c = g_pending[0];
    memmove(&g_pending[0], &g_pending[1], (size_t) (g_pending_count - 1) * sizeof(PendingChoice));
    g_pending_count--;
    show_choice(c.title, c.message, (const char *const *) c.buttons, c.count, c.cb, c.user);
    free(c.title);
    free(c.message);
    for (int i = 0; i < c.count; i++) free(c.buttons[i]);
}

static void close_dialog_ex(const char *value, const int run_next) {
    const HtmlElem el = html_dom_by_id("knPrompt");
    if (el) html_dom_remove(el);
    EdPromptFn cb = g_cb;
    void *user = g_user;
    EdChoiceFn ccb = g_choice_cb;
    void *cuser = g_choice_user;
    g_cb = NULL;
    g_user = NULL;
    g_choice_cb = NULL;
    g_choice_user = NULL;
    if (cb) cb(user, value);
    if (ccb) ccb(cuser, value ? atoi(value) : -1);
    if (run_next) next_pending();
}

static void close_dialog(const char *value) { close_dialog_ex(value, 1); }

void ed_prompt_reset(void) {
    // page leave: the document goes away; drop the dialogs without answering them
    g_cb = NULL;
    g_user = NULL;
    g_choice_cb = NULL;
    g_choice_user = NULL;
    for (int i = 0; i < g_pending_count; i++) {
        free(g_pending[i].title);
        free(g_pending[i].message);
        for (int k = 0; k < g_pending[i].count; k++) free(g_pending[i].buttons[k]);
    }
    g_pending_count = 0;
}

static void show_choice(const char *title, const char *message, const char *const *buttons, const int count, const EdChoiceFn cb, void *user) {
    g_choice_cb = cb;
    g_choice_user = user;
    EdBuf h;
    edb_init(&h);
    edb_add(&h, "<div id='knPrompt'><div class='knPromptBox knChoiceBox'><div class='knPromptTitle'>");
    edb_add_escaped(&h, title ? title : "Krunker Editor says");
    edb_add(&h, "</div><div class='knPromptMsg'>");
    // one line per "\n" (explicit breaks: the message lists scopes / projects)
    for (const char *line = message ? message : ""; line;) {
        const char *nl = strchr(line, '\n');
        char part[512];
        const size_t len = nl ? (size_t) (nl - line) : strlen(line);
        snprintf(part, sizeof(part), "%.*s", (int) (len < sizeof(part) ? len : sizeof(part) - 1), line);
        edb_add_escaped(&h, part);
        if (nl) edb_add(&h, "<br>");
        line = nl ? nl + 1 : NULL;
    }
    edb_add(&h, "</div><div class='knPromptButtons'>");
    for (int i = 0; i < count; i++) {
        edb_addf(&h, "<div class='knPromptBtn%s' onclick='KE.kn.choice(%d)'>", i == 0 ? " knPromptOk" : "", i);
        edb_add_escaped(&h, buttons[i]);
        edb_add(&h, "</div>");
    }
    edb_add(&h, "</div></div></div>");
    html_dom_insert_html(html_dom_body(), HTML_INSERT_BEFORE_END, h.buf);
    edb_free(&h);
}

void ed_choice(const char *title, const char *message, const char *const *buttons, int count, const EdChoiceFn cb, void *user) {
    if (count > 4) count = 4;
    if (!ed_prompt_open()) {
        show_choice(title, message, buttons, count, cb, user);
        return;
    }
    if (g_pending_count >= MAX_PENDING) {
        if (cb) cb(user, -1);
        return;
    }
    PendingChoice *c = &g_pending[g_pending_count++];
    c->title = strdup(title ? title : "");
    c->message = strdup(message ? message : "");
    for (int i = 0; i < count; i++) c->buttons[i] = strdup(buttons[i]);
    c->count = count;
    c->cb = cb;
    c->user = user;
}

static void open_dialog(const char *message, const char *def, const int confirm, EdPromptFn cb, void *user) {
    if (ed_prompt_open()) close_dialog_ex(NULL, 0); // replaced: queued choices wait for this one
    g_cb = cb;
    g_user = user;
    g_confirm = confirm;
    EdBuf h;
    edb_init(&h);
    edb_add(&h, "<div id='knPrompt'><div class='knPromptBox'><div class='knPromptTitle'>Krunker Editor says</div><div class='knPromptMsg'>");
    edb_add_escaped(&h, message ? message : "");
    edb_add(&h, "</div>");
    if (!confirm) {
        edb_add(&h, "<input id='knPromptInput' type='text' class='knPromptInput' onkeydown='KE.kn.promptKey()' value='");
        edb_add_escaped(&h, def ? def : "");
        edb_add(&h, "'>");
    }
    edb_add(&h, "<div class='knPromptButtons'><div class='knPromptBtn knPromptOk' onclick='KE.kn.promptOk()'>OK</div>"
                "<div class='knPromptBtn' onclick='KE.kn.promptCancel()'>Cancel</div></div></div></div>");
    html_dom_insert_html(html_dom_body(), HTML_INSERT_BEFORE_END, h.buf);
    edb_free(&h);
    const HtmlElem input = html_dom_by_id("knPromptInput");
    if (input) html_dom_focus(input);
}

void ed_prompt(const char *message, const char *def, const EdPromptFn cb, void *user) { open_dialog(message, def, 0, cb, user); }

void ed_confirm(const char *message, const EdPromptFn cb, void *user) { open_dialog(message, NULL, 1, cb, user); }

void ed_prompt_cancel(void) {
    if (ed_prompt_open()) close_dialog(NULL);
}

static void accept(void) {
    if (g_confirm) {
        close_dialog("1");
        return;
    }
    const HtmlElem input = html_dom_by_id("knPromptInput");
    const int n = input ? html_dom_get_value(input, NULL, 0) : 0;
    char *v = calloc((size_t) (n > 0 ? n : 0) + 1, 1);
    if (!v) {
        close_dialog(NULL);
        return;
    }
    if (input && n > 0) html_dom_get_value(input, v, (size_t) n + 1);
    close_dialog(v);
    free(v);
}

// ---------------------------------------------------------------------------
// window.genericPopup / resetPops / clearPops (E:1101-1128): Krunker's own popups (teleport,
// import, ...) in #popupHolder > #genericPop, closed by a click on #popupBack

static char g_popup_class[64];

static void reset_pops(void) {
    html_dom_set_display(html_dom_by_id("popupHolder"), "block");
    const HtmlElem content = html_dom_by_id("popupContent");
    html_dom_set_display(content, "none");
    html_dom_set_style(content, "background-color", "rgba(0, 0, 0, 0.75)");
    const HtmlElem pop = html_dom_by_id("genericPop");
    html_dom_set_display(pop, "none");
    html_dom_remove_attr(pop, "class");
    html_dom_set_html(pop, "");
    g_popup_class[0] = 0;
}

HtmlElem ed_generic_popup(const char *html, const char *cls) {
    reset_pops();
    const HtmlElem pop = html_dom_by_id("genericPop");
    if (!pop) return 0;
    if (cls && cls[0]) html_dom_set_attr(pop, "class", cls);
    snprintf(g_popup_class, sizeof(g_popup_class), "%s", cls ? cls : "");
    html_dom_set_attr(pop, "style", "");
    html_dom_set_html(pop, html ? html : "");
    html_dom_set_display(pop, "block");
    return pop;
}

void ed_clear_pops(void) {
    reset_pops();
    html_dom_set_display(html_dom_by_id("popupHolder"), "none");
}

int ed_popup_is(const char *cls) {
    char d[16] = "";
    html_dom_get_style(html_dom_by_id("popupHolder"), "display", d, sizeof(d));
    return !strcmp(d, "block") && !strcmp(g_popup_class, cls);
}

static int h_clear_pops(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u; (void) ev; (void) a; (void) n;
    ed_clear_pops();
    return 1;
}

static int h_prompt(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u; (void) a; (void) n;
    const char *name = html_ui_current_handler();
    if (!name) return 1;
    if (!strcmp(name, "KE.kn.choice")) {
        char v[16];
        snprintf(v, sizeof(v), "%d", n >= 1 && a[0].kind == HTML_ARG_NUM ? (int) a[0].num : -1);
        close_dialog(v);
    } else if (!strcmp(name, "KE.kn.promptOk")) accept();
    else if (!strcmp(name, "KE.kn.promptCancel")) close_dialog(NULL);
    else if (!strcmp(name, "KE.kn.promptKey")) {
        if (html_key_is_enter(ev->key)) accept();
        else if (html_key_is_escape(ev->key)) close_dialog(NULL);
    }
    return 1;
}

void ed_prompt_install(HtmlHandlerEntry *table, size_t *count, const size_t max) {
    const HtmlHandlerEntry entries[] = {
        {"KE.kn.promptOk", h_prompt},
        {"KE.kn.promptCancel", h_prompt},
        {"KE.kn.promptKey", h_prompt},
        {"KE.kn.choice", h_prompt},
        {"clearPops", h_clear_pops},
    };
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]) && *count < max; i++) table[(*count)++] = entries[i];
}
