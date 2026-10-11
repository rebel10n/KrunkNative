// Map editor: window.prompt() / window.confirm() stand-ins. Krunker's editor asks for text
// with the browser's modal dialogs (Colorize > Input, Grouping > Color / Custom, Tools >
// Text Generator, Prefab > Import > JSON, the Quick GUI's JSON entries, ...). There is no
// such dialog natively, so both platforms show the same small in-page dialog (#knPrompt,
// styled like Chromium's in ui/css/editor-kn.css) and call back asynchronously: OK passes
// the field's text (confirm: "1"), Cancel / Escape passes NULL. One dialog at a time; a new
// request cancels the open one.
#include "editor_internal.h"
#include <html_ui.h>
#include <stdlib.h>
#include <string.h>

static EdPromptFn g_cb;
static void *g_user;
static int g_confirm;

int ed_prompt_open(void) { return g_cb != NULL || html_dom_by_id("knPrompt") != 0; }

static void close_dialog(const char *value) {
    const HtmlElem el = html_dom_by_id("knPrompt");
    if (el) html_dom_remove(el);
    EdPromptFn cb = g_cb;
    void *user = g_user;
    g_cb = NULL;
    g_user = NULL;
    if (cb) cb(user, value);
}

static void open_dialog(const char *message, const char *def, const int confirm, EdPromptFn cb, void *user) {
    if (ed_prompt_open()) close_dialog(NULL);
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

static int h_prompt(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u; (void) a; (void) n;
    const char *name = html_ui_current_handler();
    if (!name) return 1;
    if (!strcmp(name, "KE.kn.promptOk")) accept();
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
    };
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]) && *count < max; i++) table[(*count)++] = entries[i];
}
