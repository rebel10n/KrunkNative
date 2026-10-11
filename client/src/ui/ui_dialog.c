// Browser dialogs Krunker v3.1.1 uses from the menu (window.confirm / prompt / alert:
// resetSettings, selectSettingPre(0), importSettings, import errors, claimReward) and
// external navigation (openURL / openTab / window.open / <a href> to other pages).
//
// Web build: the real browser dialogs and window.open, exactly like the real page.
// Native build: the page's own confirmation popup (#popupHolder > #confPop with
// #confirmBtn / #declineBtn, the markup Krunker's confKickClan & co. write), answered
// through the knDialog(1|0) handler; #popupBack / clearPops() cancels. Links open in
// the system browser (xdg-open / open / ShellExecute), http(s) only.
#include <ui_logic.h>
#include <app_mode.h>
#include <audio.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#elif defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

// Relative links on the page (/social.html, ./docs/terms.txt, ./editor.html) point at
// the site the page was served from; for KrunkNative that is Krunker's own site.
#define KRUNKER_SITE "https://krunker.io/"
#define UI_URL_MAX 1024

// ---------------------------------------------------------------- external links

// Resolve `url` like the page would and accept only http(s) results without control
// characters or whitespace. Returns 0 when the link must not be opened.
int ui_resolve_external_url(const char *url, char *out, const size_t n) {
    if (!url || !out || n < 16) return 0;
    while (*url && isspace((unsigned char) *url)) url++;
    if (!*url || *url == '#') return 0;
    int written;
    if (!strncmp(url, "https://", 8) || !strncmp(url, "http://", 7)) {
        written = snprintf(out, n, "%s", url);
    } else if (!strncmp(url, "//", 2)) {
        written = snprintf(out, n, "https:%s", url);
    } else {
        // a scheme ("javascript:", "file:", "data:", ...) before any '/' is not a path
        const size_t scheme = strcspn(url, ":/?#");
        if (url[scheme] == ':') return 0;
        if (!strncmp(url, "./", 2)) url += 2;
        while (*url == '/') url++;
        written = snprintf(out, n, "%s%s", KRUNKER_SITE, url);
    }
    if (written <= 0 || (size_t) written >= n) return 0;
    for (const char *p = out; *p; p++) {
        if ((unsigned char) *p <= 0x20 || *p == 0x7f || *p == '"' || *p == '\\') return 0;
    }
    return 1;
}

// window.open(url, "_blank"): a new browser tab.
void ui_open_external(const char *url) {
    // Krunker's own pages that are ported into the client (social hub, editor, viewer)
    // open in place instead of a browser tab (app_mode.c).
    if (app_route_url(url)) return;
    char resolved[UI_URL_MAX];
    if (!ui_resolve_external_url(url, resolved, sizeof(resolved))) {
        menu_log("link not opened (not http/https): %s", url ? url : "");
        return;
    }
#ifdef __EMSCRIPTEN__
    EM_ASM({ try { (window.knRealOpen || window.open)(UTF8ToString($0), '_blank'); } catch (e) {} }, resolved);
#else
    // KRUNKNATIVE_NO_BROWSER=1 (headless test runs): log only.
    const char *off = getenv("KRUNKNATIVE_NO_BROWSER");
    if (off && *off && strcmp(off, "0")) {
        menu_log("open (disabled by KRUNKNATIVE_NO_BROWSER): %s", resolved);
        return;
    }
    menu_log("open in browser: %s", resolved);
#if defined(_WIN32)
    ShellExecuteA(NULL, "open", resolved, NULL, NULL, SW_SHOWNORMAL);
#else
#if defined(__APPLE__)
    const char *opener = "open";
#else
    const char *opener = "xdg-open";
#endif
    // Double fork so the opener is re-parented and never left as a zombie; no shell is
    // involved (argv), and the URL was checked above.
    const pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            setsid();
            execlp(opener, opener, resolved, (char *) NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0) waitpid(pid, NULL, 0);
#endif
#endif
}

// ---------------------------------------------------------------- dialogs

typedef struct {
    int active;
    int kind;            // 0 confirm, 1 prompt, 2 alert
    UiDialogFn fn;
    void *user;
} Dialog;

static Dialog g_dialog;

#ifndef __EMSCRIPTEN__
static void set_disp(const char *id, const char *value) {
    HtmlElem el = html_dom_by_id(id);
    if (el) html_dom_set_display(el, value);
}

static void dialog_show(const char *text, const char *field, const char *yes, const char *no) {
    // resetPops(): only the holder and our popup are visible
    static const char *pops[] = {"popupContent", "confPremiumPop", "gameCodePop", "tradePop", "twitchPop", "pfpPop", "dropsPop"};
    for (size_t i = 0; i < sizeof(pops) / sizeof(pops[0]); i++) set_disp(pops[i], "none");
    ui_loadout_hide_police_pop();
    set_disp("popupHolder", "block");
    HtmlElem pop = html_dom_by_id("confPop");
    if (!pop) return;
    StrBuf sb = {0};
    sb_append(&sb, "<div style='color:#fff;font-size:20px;position:relative;margin-bottom:15px;white-space:pre-wrap'>");
    sb_append_esc(&sb, text);
    sb_append(&sb, "</div>");
    if (field) sb_append(&sb, field);
    sb_append(&sb, "<div id='confirmBtn' onclick='knDialog(1)' onmouseover='SOUND.play(&quot;tick_0&quot;,0.1)'>");
    sb_append_esc(&sb, yes);
    sb_append(&sb, "</div>");
    if (no) {
        sb_append(&sb, "<div id='declineBtn' onclick='knDialog(0)' onmouseover='SOUND.play(&quot;tick_0&quot;,0.1)'>");
        sb_append_esc(&sb, no);
        sb_append(&sb, "</div>");
    }
    char *html = sb_finish(&sb);
    html_dom_set_html(pop, html);
    free(html);
    html_dom_set_display(pop, "block");
    HtmlElem input = html_dom_by_id("knPromptInput");
    if (input) html_dom_focus(input);
    menu_schedule_rect(); // the preview cut-out follows #popupBack (menu_ui.c)
}

static void dialog_hide(void) {
    set_disp("confPop", "none");
    set_disp("popupHolder", "none");
    menu_schedule_rect();
}
#endif

static void dialog_begin(const int kind, UiDialogFn fn, void *user) {
    if (g_dialog.active) ui_dialog_answer(0); // a new dialog cancels the pending one
    g_dialog.active = 1;
    g_dialog.kind = kind;
    g_dialog.fn = fn;
    g_dialog.user = user;
}

// knDialog(accepted): the popup's buttons.
void ui_dialog_answer(const int accepted) {
    if (!g_dialog.active) return;
    Dialog d = g_dialog;
    memset(&g_dialog, 0, sizeof(g_dialog));
    char text[UI_SETTINGS_IMPORT_MAX] = "";
#ifndef __EMSCRIPTEN__
    HtmlElem input = html_dom_by_id("knPromptInput");
    if (d.kind == 1 && accepted && input) html_dom_get_value(input, text, sizeof(text));
    dialog_hide();
#endif
    if (d.fn) d.fn(accepted, text, d.user);
}

// clearPops(): the backdrop click dismisses the dialog like "Cancel".
void ui_dialog_cancel(void) {
    if (g_dialog.active) ui_dialog_answer(0);
}

int ui_dialog_prompt_focused(const HtmlElem el) {
    return g_dialog.active && g_dialog.kind == 1 && el && el == html_dom_by_id("knPromptInput");
}

void ui_confirm(const char *text, UiDialogFn fn, void *user) {
#ifdef __EMSCRIPTEN__
    const int ok = EM_ASM_INT({ return window.confirm(UTF8ToString($0)) ? 1 : 0; }, text ? text : "");
    if (fn) fn(ok, "", user);
#else
    dialog_begin(0, fn, user);
    dialog_show(text ? text : "", NULL, "YES", "NO");
#endif
}

void ui_prompt(const char *text, UiDialogFn fn, void *user) {
#ifdef __EMSCRIPTEN__
    char *answer = (char *) (intptr_t) EM_ASM_INT({
        var v = window.prompt(UTF8ToString($0));
        return v == null ? 0 : stringToNewUTF8(v);
    }, text ? text : "");
    if (fn) fn(answer != NULL, answer ? answer : "", user);
    free(answer);
#else
    dialog_begin(1, fn, user);
    dialog_show(text ? text : "",
                "<input id='knPromptInput' type='text' class='accountInput' autocomplete='off' style='margin-top:0;margin-bottom:10px;background-color:#fff;color:rgba(0,0,0,0.8)'>",
                "OK", "Cancel");
#endif
}

void ui_alert(const char *text) {
    menu_log("alert: %s", text ? text : "");
#ifdef __EMSCRIPTEN__
    EM_ASM({ window.alert(UTF8ToString($0)); }, text ? text : "");
#else
    dialog_begin(2, NULL, NULL);
    dialog_show(text ? text : "", NULL, "OK", NULL);
#endif
}
