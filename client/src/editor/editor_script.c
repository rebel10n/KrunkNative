// KrunkScript code window (doc/v7/editor.md WP5, section 1.8): Editor.scriptPopup /
// updateScript / updateTab / io_ksC (E:2217-2284, 1946-1991) and the page they open,
// scripting.html (recovered from krunker.io by tools/editor/fetch_scripting_ref.sh;
// editor_script_lang.h has its mode and hint rules, ui/css/editor-script.css its CSS).
//
// Intentional differences from v7.1.4:
// - The window is an in-page window (#knScriptWin over the editor, at v7's window.open
//   geometry: 10% / 10%, 80% x 80%) with a close button, not a second browser window: every
//   KrunkNative page is its own process and the HTML engine has one document.
// - CodeMirror is the engine's code view (html_dom_code_view: a <textarea> with the line-number
//   gutter, colours, caret line and marks of the material-palenight theme); folding (Ctrl-Q,
//   the fold-gutter clicks) and the find dialog (Alt-F) are not ported, the fold gutter shows
//   its markers only.
// - Validation is local and immediate (shared/src/ks ks_validate, the checker ksc uses) instead
//   of the "ksC" socket round trip: no "Compiling..." / "Validation Pending" / "Please Reconnect"
//   states. Diagnostics also run live while typing (the error line is marked as v7's
//   highlightErrors marks it) and are listed under the code (#knScriptErrors; v7 showed the
//   error in an alert()); the status button and the tab icons change on Validate only, as in v7.
// - The code is kept in the project as it is typed (300 ms debounce), not only on Validate / tab
//   switch / window close, so an export never misses edits; "Script Saved" still shows on those.
// - The def rule's "(" counts for auto-indent and show-hint's alias rewrite only touches the
//   entries it matched (editor_script_lang.c).
#include "editor_internal.h"
#include "editor_model.h"
#include "editor_script_lang.h"

#include <ks.h>
#include <ks_import_js.h>
#include <ui_logic.h>
#include <GLFW/glfw3.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// material-palenight (CodeMirror 5.61.1 theme) + scripting.css, 0xRRGGBBAA
#define PAL_TEXT 0xffffffffu     // .CodeMirror-line span {color:#fff}
#define PAL_GUTTER_BG 0x292d3effu
#define PAL_GUTTER_FG 0x676e95ffu
#define PAL_CARET 0xffcc00ffu
#define PAL_SELECTION 0x717cb433u   // rgba(113,124,180,.2)
#define PAL_ACTIVE_LINE 0x00000033u // scripting.css: rgba(0,0,0,0.2) !important
#define PAL_ERROR_LINE 0xff00004du  // .code-error rgba(255,0,0,0.3)
#define PAL_BRACKET 0xffffffffu     // .CodeMirror-matchingbracket: underline, #fff
#define PAL_NO_BRACKET 0xaa2222ffu  // codemirror.css .CodeMirror-nonmatchingbracket #a22
static const uint32_t k_style_color[EKS__COUNT] = {
    PAL_TEXT,
    0x676e95ffu, // comment
    0xc792eaffu, // keyword
    0x82aaffffu, // def
    0xdecb6bffu, // type (the theme's second .cm-type rule wins)
    0xf78c6cffu, // atom
    0xf07178ffu, // variable
    0xff5370ffu, // number
    0xc3e88dffu, // string
};

#define SW_MAX_DIAGS 17
#define SW_HINT_ROWS_EM 288.0f // .CodeMirror-hints max-height 20em at 90% of 16px

typedef struct {
    int open;
    int tab;                    // KE.currentKey: 0 client, 1 server
    HtmlElem win, ta[2], tab_el[2], icon[2], hints, errors, compile;
    char *seen[2];              // the value the window last processed (input diffs)
    int seen_a[2], seen_b[2];   // its selection
    int errored[2], validated[2]; // KE.erroredScripts / validatedScripts
    int changed[2];             // edited since the last Validate: the grey circle (iconC)
    int pending[2];             // edited, not yet kept in the project
    double store_at;            // when to keep + check them (g_ed.now)
    EdKsDiag diags[2][SW_MAX_DIAGS];
    int ndiags[2];
    EdKsHint *hint;             // the open hint list (show-hint)
    int nhint, hint_sel, hint_top, hint_from, hint_open;
    int poll_a, poll_b;         // cursorActivity poll
    EditorProject *proj;        // whose scripts are shown
    unsigned proj_rev;
    int font;                   // settings.scriptFont as applied
    int compile_timer, saved_timer;
    int in_edit;                // our own replaces (the web fires input for them)
    int open_digits[2];         // line-number digits when the window opened (view_for)
    int view_digits[2];         // the code view as applied: digits, font
    int view_font[2];
} ScriptWin;

static ScriptWin g_sw;

static const char *side_name(const int side) { return side ? "server" : "client"; }

static char *get_value(const HtmlElem e) {
    const int n = html_dom_get_value(e, NULL, 0);
    char *v = malloc((size_t) (n > 0 ? n : 0) + 1);
    if (!v) return NULL;
    v[0] = 0;
    if (n > 0) html_dom_get_value(e, v, (size_t) n + 1);
    return v;
}

static int project_alive(const EditorProject *p) {
    for (size_t i = 0; p && i < g_ed.project_count; i++) {
        if (g_ed.projects[i] == p) return 1;
    }
    return 0;
}

static int dom_key(const HtmlEvent *ev) {
#ifdef __EMSCRIPTEN__
    return ev->key; // the browser's keyCode
#else
    return ui_controls_dom_key(ev->key);
#endif
}

// ---------------------------------------------------------------------------
// markup (scripting.html's body, scoped under #knScriptWin; ui/css/editor-script.css)

static void build(void) {
    HtmlElem w = html_dom_by_id("knScriptWin");
    if (!w) {
        html_dom_insert_html(html_dom_body(), HTML_INSERT_BEFORE_END,
            "<div id='knScriptWin' style='display:none'>"
            "<div id='tabHolder'>"
            "<div id='client' class='tab selected' onclick='KE.kn.scriptTab(0)'><span id='tabIcon_client' class='tabIcon'></span>"
            "<div style='float:left;margin-top:2px'>Client</div></div>"
            "<div id='server' class='tab' onclick='KE.kn.scriptTab(1)'><span id='tabIcon_server' class='tabIcon'></span>"
            "<div style='float:left;margin-top:2px'>Server</div></div>"
            "<div id='knScriptClose' class='material-icons' onclick='KE.kn.scriptClose()'>close</div>"
            "<div id='label'><a style='color: #fff'>KRUNK</a>SCRIPT</div>"
            "</div>"
            "<textarea id='scriptArea_client' class='knCode' spellcheck='false' wrap='off' oninput='KE.kn.scriptInput(0)' "
            "onkeydown='return KE.kn.scriptKey(0)' onblur='KE.kn.scriptBlur(0)'></textarea>"
            "<textarea id='scriptArea_server' class='knCode' spellcheck='false' wrap='off' style='display:none' oninput='KE.kn.scriptInput(1)' "
            "onkeydown='return KE.kn.scriptKey(1)' onblur='KE.kn.scriptBlur(1)'></textarea>"
            "<ul id='knScriptHints' class='CodeMirror-hints' style='display:none'></ul>"
            "<div id='knScriptErrors' style='display:none'></div>"
            "<div id='compile' onclick='KE.updateScript()'>Validate Script</div>"
            "</div>");
        w = html_dom_by_id("knScriptWin");
    }
    g_sw.win = w;
    g_sw.ta[0] = html_dom_by_id("scriptArea_client");
    g_sw.ta[1] = html_dom_by_id("scriptArea_server");
    g_sw.tab_el[0] = html_dom_query(w, "#client");
    g_sw.tab_el[1] = html_dom_query(w, "#server");
    g_sw.icon[0] = html_dom_by_id("tabIcon_client");
    g_sw.icon[1] = html_dom_by_id("tabIcon_server");
    g_sw.hints = html_dom_by_id("knScriptHints");
    g_sw.errors = html_dom_by_id("knScriptErrors");
    g_sw.compile = html_dom_query(w, "#compile");
}

static int digits(int n) {
    int d = 1;
    while (n >= 10) {
        n /= 10;
        d++;
    }
    return d;
}

static int count_lines(const char *v) {
    int n = 1;
    for (const char *p = v; p && *p; p++) n += *p == '\n';
    return n;
}

// The code view of a tab: CodeMirror lineNumbers + foldGutter (.CodeMirror-linenumber padding
// 0 3px 0 5px, min-width 20px; .CodeMirror-foldgutter .7em; pre.CodeMirror-line padding 0 4px).
// v7's page builds CodeMirror at scripting.css's 22px and only then setFont(scriptFont), so the
// gutter width CodeMirror measured (lineNumInnerWidth = max(20, digits) + 1, the sizer's margin =
// the gutters' width) stays the 22px one until the line count gains a digit: with the default
// 13px font the numbers end at 32px and the text starts at 54px. Matched while the digit count is
// the one the window opened with (the monospace advance is 0.6em).
static void view_for(const int side, const int lines) {
    const int font = g_sw.font > 0 ? g_sw.font : 22;
    const int d = digits(lines);
    if (d == g_sw.view_digits[side] && font == g_sw.view_font[side]) return;
    g_sw.view_digits[side] = d;
    g_sw.view_font[side] = font;
    const int stale = d == g_sw.open_digits[side];
    const float at = stale ? 22.0f : (float) font;
    const float inner = (float) (int) (fmaxf(20.0f, 0.6f * at * (float) d) + 0.5f) + 1.0f; // offsetWidth + 1
    const float fold = 0.7f * (float) font;
    const float sizer = stale ? (float) (int) (inner + 8.0f + 0.7f * 22.0f + 0.5f) : inner + 8.0f + fold;
    HtmlCodeView cv = {
        .tab_size = ED_KS_TAB_SIZE, .gutter = 1, .gutter_fg = PAL_GUTTER_FG, .gutter_bg = PAL_GUTTER_BG,
        .gutter_pad_left = 5, .gutter_pad_right = 3, .gutter_min_w = inner, .gutter_extra = fold,
        .gutter_gap = sizer - (inner + 8.0f + fold), .text_pad = 4, .active_line_bg = PAL_ACTIVE_LINE, .caret = PAL_CARET, .selection_bg = PAL_SELECTION,
    };
    html_dom_code_view(g_sw.ta[side], &cv);
}

// setFont: .CodeMirror and the hint rows (scripting.css rules 4 / 5) at settings.scriptFont
static void apply_font(void) {
    int px = (int) editor_setting_num("scriptFont", 0);
    if (px <= 0) px = 22; // this.settings.scriptFont || 22
    if (px == g_sw.font) return;
    g_sw.font = px;
    char v[32];
    snprintf(v, sizeof(v), "%dpx", px);
    for (int i = 0; i < 2; i++) {
        html_dom_set_style(g_sw.ta[i], "font-size", v);
        char *t = get_value(g_sw.ta[i]);
        view_for(i, count_lines(t));
        free(t);
    }
    html_dom_set_style(g_sw.hints, "font-size", v);
}

// ---------------------------------------------------------------------------
// decorations: token colours, brackets, error lines, fold markers

typedef struct {
    HtmlCodeSpan *v;
    int n, cap;
} SpanList;

static void span_push(SpanList *l, const int a, const int b, const uint32_t color, const int flags) {
    if (b <= a) return;
    if (l->n == l->cap) {
        const int cap = l->cap ? l->cap * 2 : 256;
        HtmlCodeSpan *nv = realloc(l->v, (size_t) cap * sizeof(*nv));
        if (!nv) return;
        l->v = nv;
        l->cap = cap;
    }
    l->v[l->n++] = (HtmlCodeSpan) {a, b, color, flags};
}

// the token spans with a one-character mark at m0 / m1 (brackets) cut into them
static void spans_with_marks(SpanList *l, const EdKsToken *t, const int n, const int m0, const int m1, const uint32_t mc, const int mflags) {
    int marks[2] = {m0 < m1 || m1 < 0 ? m0 : m1, m0 < m1 || m1 < 0 ? m1 : m0};
    int mi = 0;
    while (mi < 2 && marks[mi] < 0) mi++;
    for (int i = 0; i < n; i++) {
        int a = t[i].start;
        const int b = t[i].end;
        const uint32_t c = k_style_color[t[i].style];
        // marks before this token
        while (mi < 2 && marks[mi] >= 0 && marks[mi] < a) {
            span_push(l, marks[mi], marks[mi] + 1, mc, mflags);
            mi++;
        }
        while (mi < 2 && marks[mi] >= a && marks[mi] < b) {
            span_push(l, a, marks[mi], c, 0);
            span_push(l, marks[mi], marks[mi] + 1, mc, mflags);
            a = marks[mi] + 1;
            mi++;
        }
        span_push(l, a, b, c, 0);
    }
    for (; mi < 2; mi++) {
        if (marks[mi] >= 0) span_push(l, marks[mi], marks[mi] + 1, mc, mflags);
    }
}

// brace-fold's range finder: the last { (else [) of the line outside strings / comments,
// whose partner is on a later line
static int line_folds(const char *v, const size_t len, const EdKsToken *t, const int n, const int ls, const int le) {
    for (int pass = 0; pass < 2; pass++) {
        const char want = pass ? '[' : '{';
        for (int i = le - 1; i >= ls; i--) {
            if (v[i] != want) continue;
            const int st = ed_ks_style_at(t, n, i);
            if (st == EKS_COMMENT || st == EKS_STRING) continue;
            int a, b;
            if (ed_ks_match_bracket(v, len, t, n, i + 1, &a, &b) == 1 && a == i && b > le) return 1;
            break;
        }
    }
    return 0;
}

static void decorate(const int side) {
    const HtmlElem ta = g_sw.ta[side];
    char *v = get_value(ta);
    if (!v) return;
    const size_t len = strlen(v);
    int n = ed_ks_tokenize(v, len, NULL, 0);
    EdKsToken *t = malloc((size_t) (n > 0 ? n : 1) * sizeof(*t));
    if (!t) {
        free(v);
        return;
    }
    n = ed_ks_tokenize(v, len, t, n);
    // matchBrackets (only with an empty selection)
    int sa = 0, sb = 0, ba = -1, bb = -1, match = 0;
    html_dom_text_selection(ta, &sa, &sb);
    if (sa == sb) match = ed_ks_match_bracket(v, len, t, n, sb, &ba, &bb);
    if (!match) ba = bb = -1;
    SpanList spans = {0};
    spans_with_marks(&spans, t, n, ba, bb, match > 0 ? PAL_BRACKET : PAL_NO_BRACKET, match > 0 ? HTML_CODE_UNDERLINE : 0);
    html_dom_code_spans(ta, spans.v, spans.n);
    free(spans.v);
    // lines: the error lines (highlightErrors: markText line-1 .. line, "code-error") and the
    // fold gutter's open markers (U+25BE, .CodeMirror-guttermarker-subtle)
    const int nl = count_lines(v);
    view_for(side, nl);
    HtmlCodeLine *lines = calloc((size_t) nl + SW_MAX_DIAGS, sizeof(*lines));
    int k = 0;
    for (int ln = 0, ls = 0; lines && ln < nl; ln++) {
        int le = ls;
        while (le < (int) len && v[le] != '\n') le++;
        if (line_folds(v, len, t, n, ls, le)) {
            lines[k].line = ln + 1;
            lines[k].marker = 0x25BE;
            lines[k].marker_color = PAL_GUTTER_FG;
            k++;
        }
        ls = le + 1;
    }
    for (int i = 0; lines && i < g_sw.ndiags[side]; i++) {
        const EdKsDiag *d = &g_sw.diags[side][i];
        if (!d->error) continue;
        const int line = d->line > 0 ? d->line : 1;
        int j = 0;
        while (j < k && lines[j].line != line) j++;
        if (j == k) lines[k++].line = line;
        lines[j].bg = PAL_ERROR_LINE;
    }
    html_dom_code_lines(ta, lines, k);
    free(lines);
    free(t);
    free(v);
}

// #knScriptErrors (KN): the diagnostics of the shown tab, click = go to the place
static void render_errors(void) {
    const int side = g_sw.tab;
    if (!g_sw.ndiags[side]) {
        html_dom_set_html(g_sw.errors, "");
        html_dom_set_display(g_sw.errors, "none");
        html_dom_set_class(g_sw.win, "knHasDiags", 0);
        return;
    }
    EdBuf b;
    edb_init(&b);
    for (int i = 0; i < g_sw.ndiags[side]; i++) {
        const EdKsDiag *d = &g_sw.diags[side][i];
        edb_addf(&b, "<div class='knDiag %s' onclick='KE.kn.scriptGoto(%d,%d)'><span class='knDiagPos'>", d->error ? "knDiagE" : "knDiagW", d->line, d->col);
        if (d->line > 0) edb_addf(&b, "line %d:%d", d->line, d->col);
        edb_addf(&b, "</span> %s ", d->error ? "error" : "warning");
        edb_add_escaped(&b, d->message);
        edb_add(&b, "</div>");
    }
    html_dom_set_html(g_sw.errors, b.buf ? b.buf : "");
    html_dom_set_display(g_sw.errors, "");
    html_dom_set_class(g_sw.win, "knHasDiags", 1);
    edb_free(&b);
}

static void run_check(const int side) {
    char *v = get_value(g_sw.ta[side]);
    g_sw.ndiags[side] = v ? ed_ks_check(v, side, g_sw.diags[side], SW_MAX_DIAGS) : 0;
    free(v);
    decorate(side);
    if (side == g_sw.tab) render_errors();
}

// ---------------------------------------------------------------------------
// keeping the code in the project (KE.scripts[key].code)

static void keep(const int side) {
    if (!g_sw.proj || !project_alive(g_sw.proj)) return;
    char *v = get_value(g_sw.ta[side]);
    if (!v) return;
    EditorProject *prev;
    ed_project_enter(g_sw.proj, &prev);
    if (strcmp(v, ed_ks_script_text(g_sw.proj->scripts, side))) {
        ed_ks_script_store(g_sw.proj->scripts, side, v);
        ed_mark_changed();
    }
    g_sw.proj_rev = g_sw.proj->revision;
    ed_project_leave(prev);
    g_sw.pending[side] = 0;
    free(v);
}

static void flush_pending(void) {
    for (int i = 0; i < 2; i++) {
        if (g_sw.pending[i]) {
            keep(i);
            run_check(i);
        }
    }
}

static void set_seen(const int side) {
    free(g_sw.seen[side]);
    g_sw.seen[side] = get_value(g_sw.ta[side]);
    html_dom_text_selection(g_sw.ta[side], &g_sw.seen_a[side], &g_sw.seen_b[side]);
}

// the project's scripts into the textareas (code || templates[key])
static void load_project(void) {
    g_sw.proj = g_ed.proj;
    g_sw.proj_rev = g_ed.proj ? g_ed.proj->revision : 0;
    for (int i = 0; i < 2; i++) {
        const char *code = g_ed.proj ? ed_ks_script_text(g_ed.proj->scripts, i) : ed_model_script_template(side_name(i));
        g_sw.in_edit++;
        html_dom_set_value(g_sw.ta[i], code);
        html_dom_text_set_selection(g_sw.ta[i], 0, 0); // CodeMirror setValue: the cursor at the start
        g_sw.in_edit--;
        g_sw.pending[i] = 0;
        g_sw.open_digits[i] = digits(count_lines(code));
        g_sw.view_digits[i] = 0; // re-applied by run_check -> decorate
        set_seen(i);
        run_check(i);
    }
}

// ---------------------------------------------------------------------------
// tabs, status button, tab icons (scripting.html changeTab / updateTabIcons, f964)

static void set_compile(const char *text, const char *bg) {
    html_dom_set_text(g_sw.compile, text);
    html_dom_set_style(g_sw.compile, "background-color", bg ? bg : "");
}

// updateTabIcons(key, errored, validated, changed): scripting.html (2025) calls it with
// changed = true on every CodeMirror "change" (which also clears the other two); the editor's
// three-argument calls clear it
static void update_tab_icons(const int side) {
    html_dom_set_class(g_sw.icon[side], "iconE", g_sw.errored[side]);
    html_dom_set_class(g_sw.icon[side], "iconV", g_sw.validated[side]);
    html_dom_set_class(g_sw.icon[side], "iconC", g_sw.changed[side]);
}

static void mark_changed(const int side) {
    if (g_sw.changed[side] && !g_sw.errored[side] && !g_sw.validated[side]) return;
    g_sw.changed[side] = 1;
    g_sw.errored[side] = g_sw.validated[side] = 0;
    update_tab_icons(side);
}

static void hint_close(void);
static void focus_keep(int side);

static void show_tab(const int side) {
    g_sw.tab = side;
    for (int i = 0; i < 2; i++) {
        html_dom_set_class(g_sw.tab_el[i], "selected", i == side);
        html_dom_set_display(g_sw.ta[i], i == side ? "" : "none");
    }
    render_errors();
}

static void reset_compile_cb(void *u) {
    (void) u;
    g_sw.compile_timer = 0;
    if (g_sw.open) set_compile("Validate Script", NULL);
}

static void saved_off_cb(void *u) {
    (void) u;
    g_sw.saved_timer = 0;
    editor_warning("scriptSaving", 0, NULL, NULL);
}

static void later_reset_compile(const double ms) {
    if (g_sw.compile_timer) html_timer_cancel(g_sw.compile_timer);
    g_sw.compile_timer = html_timer_add(ms, reset_compile_cb, NULL);
}

// Editor.updateScript(saveOnly) for the shown tab (KE.currentKey)
static void update_script(const int save_only) {
    if (!g_sw.open) return;
    const int side = g_sw.tab;
    set_compile(save_only ? "Saving..." : "Compiling...", NULL);
    if (!save_only) {
        g_sw.validated[side] = g_sw.errored[side] = g_sw.changed[side] = 0;
        update_tab_icons(side);
    }
    keep(side);
    if (!save_only) {
        // io_ksC: Failed (every line of the error) / Success
        run_check(side);
        int failed = 0;
        for (int i = 0; i < g_sw.ndiags[side]; i++) failed |= g_sw.diags[side][i].error;
        if (failed) set_compile("Failed - errors found!", "#f44336");
        else set_compile("Success!", "#4caf50");
        g_sw.errored[side] = failed;
        g_sw.validated[side] = !failed;
        update_tab_icons(side);
        later_reset_compile(1000);
    }
    if (g_sw.saved_timer) html_timer_cancel(g_sw.saved_timer);
    editor_warning("scriptSaving", 0, NULL, NULL);
    editor_warning("scriptSaving", 1, "Script Saved", "save");
    g_sw.saved_timer = html_timer_add(5000, saved_off_cb, NULL);
    if (save_only) later_reset_compile(500);
}

// changeTab: the old tab is saved (updateTab -> updateScript(true)), the other one shown
static void change_tab(const int side) {
    if (side == g_sw.tab || side < 0 || side > 1) return;
    hint_close();
    update_script(1);
    show_tab(side);
    focus_keep(side);
}

// ---------------------------------------------------------------------------
// show-hint

static void hint_close(void) {
    if (!g_sw.hint_open) return;
    g_sw.hint_open = 0;
    g_sw.nhint = 0;
    html_dom_set_html(g_sw.hints, "");
    html_dom_set_display(g_sw.hints, "none");
}

static int hint_rows(void) {
    const int rows = (int) (SW_HINT_ROWS_EM / ((float) (g_sw.font > 0 ? g_sw.font : 13) * 1.2f));
    return rows < 3 ? 3 : rows;
}

static void hint_render(void) {
    const int rows = hint_rows();
    if (g_sw.hint_sel < g_sw.hint_top) g_sw.hint_top = g_sw.hint_sel;
    if (g_sw.hint_sel >= g_sw.hint_top + rows) g_sw.hint_top = g_sw.hint_sel - rows + 1;
    EdBuf b;
    edb_init(&b);
    for (int i = g_sw.hint_top; i < g_sw.nhint && i < g_sw.hint_top + rows; i++) {
        edb_addf(&b, "<li class='CodeMirror-hint%s' onmousedown='KE.kn.scriptHintPick(%d)'>", i == g_sw.hint_sel ? " CodeMirror-hint-active" : "", i);
        edb_add_escaped(&b, g_sw.hint[i].text);
        edb_add(&b, " <a>");
        edb_add_escaped(&b, g_sw.hint[i].type);
        edb_add(&b, "</a></li>");
    }
    html_dom_set_html(g_sw.hints, b.buf ? b.buf : "");
    edb_free(&b);
}

// cursorActivity -> showHint unless the hint is active (then it updates): the list for the
// word before the cursor, at the word's start (alignWithWord)
static void hint_update(const int side) {
    const HtmlElem ta = g_sw.ta[side];
    if (side != g_sw.tab || html_dom_active_element() != ta) {
        hint_close();
        return;
    }
    int a = 0, b = 0;
    html_dom_text_selection(ta, &a, &b);
    char *v = get_value(ta);
    if (!v) return;
    if (a != b) {
        free(v);
        hint_close();
        return;
    }
    int ls = b;
    while (ls > 0 && v[ls - 1] != '\n') ls--;
    int le = b;
    while (v[le] && v[le] != '\n') le++;
    if (!g_sw.hint) g_sw.hint = malloc(sizeof(EdKsHint) * ED_KS_HINT_MAX);
    const cJSON *kss = editor_data_get("kss");
    int from = 0;
    const int n = g_sw.hint ? ed_ks_hints(v + ls, (size_t) (le - ls), b - ls, cJSON_GetObjectItemCaseSensitive(kss, "autoComplete"),
                                          cJSON_GetObjectItemCaseSensitive(kss, "alias"), g_sw.hint, ED_KS_HINT_MAX, &from)
                              : 0;
    if (n <= 0) {
        free(v);
        hint_close();
        return;
    }
    const int was_open = g_sw.hint_open;
    g_sw.nhint = n;
    g_sw.hint_from = ls + from;
    if (!was_open || g_sw.hint_sel >= n) g_sw.hint_sel = 0;
    if (!was_open) g_sw.hint_top = 0;
    g_sw.hint_open = 1;
    // placement: under the word's start (the caret box moved back over the word)
    HtmlRect r;
    if (html_dom_text_caret_rect(ta, &r)) {
        // the word is ASCII ([\w.]): its width from the caret box and the code font
        const float ch = 0.6f * (float) (g_sw.font > 0 ? g_sw.font : 13); // monospace advance
        char pos[32];
        snprintf(pos, sizeof(pos), "%.1fpx", r.x - ch * (float) (b - (ls + from)));
        html_dom_set_style(g_sw.hints, "left", pos);
        snprintf(pos, sizeof(pos), "%.1fpx", r.y + r.h);
        html_dom_set_style(g_sw.hints, "top", pos);
    }
    hint_render();
    html_dom_set_display(g_sw.hints, "");
    free(v);
}

// focus() without moving the caret (a programmatic focus puts it at the end; CodeMirror keeps
// each document's selection)
static void focus_keep(const int side) {
    int a = 0, b = 0;
    html_dom_text_selection(g_sw.ta[side], &a, &b);
    if (html_dom_active_element() == g_sw.ta[side]) return;
    html_dom_focus(g_sw.ta[side]);
    html_dom_text_set_selection(g_sw.ta[side], a, b);
}

static void edit_replace(const int side, const int a, const int b, const char *text) {
    g_sw.in_edit++;
    html_dom_text_replace(g_sw.ta[side], a, b, text);
    g_sw.in_edit--;
}

static void after_edit(const int side) {
    mark_changed(side);
    set_seen(side);
    g_sw.pending[side] = 1;
    g_sw.store_at = g_ed.now + 0.3;
    decorate(side);
    g_sw.poll_a = g_sw.seen_a[side];
    g_sw.poll_b = g_sw.seen_b[side];
    hint_update(side);
}

static void hint_pick(const int index) {
    const int side = g_sw.tab;
    if (!g_sw.hint_open || index < 0 || index >= g_sw.nhint) return;
    int a = 0, b = 0;
    html_dom_text_selection(g_sw.ta[side], &a, &b);
    char text[sizeof(g_sw.hint[0].text)];
    snprintf(text, sizeof(text), "%s", g_sw.hint[index].text);
    hint_close();
    edit_replace(side, g_sw.hint_from, b, text);
    after_edit(side);
    hint_close(); // CodeMirror's pick closes it; the next cursor move may reopen it
}

// ---------------------------------------------------------------------------
// input: closebrackets on what was typed (the diff against the last value)

static void on_input(const int side) {
    if (g_sw.in_edit || !g_sw.open) return;
    const HtmlElem ta = g_sw.ta[side];
    char *v = get_value(ta);
    const char *old = g_sw.seen[side] ? g_sw.seen[side] : "";
    if (!v) return;
    const int ol = (int) strlen(old), nl = (int) strlen(v), oa = g_sw.seen_a[side], ob = g_sw.seen_b[side];
    int a = 0, b = 0;
    html_dom_text_selection(ta, &a, &b);
    // exactly one ASCII character replaced the old selection [oa, ob)?
    if (nl == ol - (ob - oa) + 1 && oa >= 0 && ob <= ol && a == b && a == oa + 1 && !memcmp(v, old, (size_t) oa) &&
        !memcmp(v + oa + 1, old + ob, (size_t) (ol - ob))) {
        const char ch = v[oa];
        if (oa != ob && ed_ks_closer(ch)) {
            // surround the selection
            char *wrap = malloc((size_t) (ob - oa) + 3);
            if (wrap) {
                wrap[0] = ch;
                memcpy(wrap + 1, old + oa, (size_t) (ob - oa));
                wrap[ob - oa + 1] = ed_ks_closer(ch);
                wrap[ob - oa + 2] = 0;
                edit_replace(side, oa, oa + 1, wrap);
                html_dom_text_set_selection(ta, oa + 1, ob + 1);
                free(wrap);
            }
        } else if (oa == ob) {
            const int act = ed_ks_close_action(old, (size_t) ol, oa, ch);
            if (act == 2) {
                edit_replace(side, oa + 1, oa + 2, ""); // typed over the closer
                html_dom_text_set_selection(ta, oa + 1, oa + 1);
            } else if (act == 1) {
                const char closer[2] = {ed_ks_closer(ch), 0};
                edit_replace(side, oa + 1, oa + 1, closer);
                html_dom_text_set_selection(ta, oa + 1, oa + 1);
            }
        }
    }
    free(v);
    after_edit(side);
}

// ---------------------------------------------------------------------------
// keys (CodeMirror keymap + show-hint + closebrackets; the page's Ctrl+S)

// lines [ls, le) touched by the selection [a, b)
static void sel_lines(const char *v, const int a, int b, int *ls, int *le) {
    if (b > a && v[b - 1] == '\n') b--;
    *ls = a;
    while (*ls > 0 && v[*ls - 1] != '\n') (*ls)--;
    *le = b;
    while (v[*le] && v[*le] != '\n') (*le)++;
}

// indentSelection("add") (Tab with a selection) / indentAuto (Shift-Tab): line by line, top
// down, on the text as re-indented so far (an opener's indentation feeds the lines after it)
static void indent_lines(const int side, const char *v, const int a, const int b, const int smart) {
    int ls, le;
    sel_lines(v, a, b, &ls, &le);
    char *work = malloc(strlen(v) + 1);
    if (!work) return;
    strcpy(work, v);
    int p = ls, end = le;
    for (;;) {
        const size_t wl = strlen(work);
        int ib;
        const int cols = ed_ks_line_indent(work, wl, p, &ib);
        const int want = smart ? ed_ks_indent_at(work, wl, p) : cols + ED_KS_TAB_SIZE;
        char ind[128];
        const int k = ed_ks_indent_string(want, ind, sizeof(ind));
        char *nw = malloc(wl - (size_t) ib + (size_t) k + 1); // work[0, p) + ind + work[p + ib, ...)
        if (!nw) break;
        memcpy(nw, work, (size_t) p);
        memcpy(nw + p, ind, (size_t) k);
        memcpy(nw + p + k, work + p + ib, wl - (size_t) (p + ib) + 1);
        free(work);
        work = nw;
        end += k - ib;
        while (work[p] && work[p] != '\n') p++;
        if (!work[p] || p >= end) break;
        p++;
    }
    const int n = end - ls;
    char *block = malloc((size_t) n + 1);
    if (block) {
        memcpy(block, work + ls, (size_t) n);
        block[n] = 0;
        edit_replace(side, ls, le, block);
        if (a != b) html_dom_text_set_selection(g_sw.ta[side], ls, ls + n);
        else {
            int ib;
            ed_ks_line_indent(block, (size_t) n, 0, &ib);
            html_dom_text_set_selection(g_sw.ta[side], ls + ib, ls + ib);
        }
        free(block);
    }
    free(work);
}

// newlineAndIndent, with closebrackets' explode between {} and []
static void newline_indent(const int side, const char *v, const int a, const int b) {
    const int len = (int) strlen(v);
    const int explode = a == b && a > 0 && b < len && ((v[a - 1] == '{' && v[b] == '}') || (v[a - 1] == '[' && v[b] == ']'));
    int ws = 0; // the whitespace the new line starts with is re-indented (smart indent)
    while (b + ws < len && (v[b + ws] == ' ' || v[b + ws] == '\t')) ws++;
    char *t = malloc((size_t) len + 4);
    if (!t) return;
    memcpy(t, v, (size_t) a);
    memcpy(t + a, explode ? "\n\n" : "\n", explode ? 2 : 1);
    strcpy(t + a + (explode ? 2 : 1), v + b + ws);
    char mid[128], close[128], out[260];
    ed_ks_indent_string(ed_ks_indent_at(t, strlen(t), a + 1), mid, sizeof(mid));
    if (explode) {
        ed_ks_indent_string(ed_ks_indent_at(t, strlen(t), a + 2), close, sizeof(close));
        snprintf(out, sizeof(out), "\n%s\n%s", mid, close);
    } else {
        snprintf(out, sizeof(out), "\n%s", mid);
    }
    free(t);
    edit_replace(side, a, b + ws, out);
    const int caret = a + 1 + (int) strlen(mid);
    html_dom_text_set_selection(g_sw.ta[side], caret, caret);
}

static int on_key(const int side, const HtmlEvent *ev) {
    if (!g_sw.open || side != g_sw.tab) return 1;
    const int key = dom_key(ev);
    const int ctrl = (ev->mods & (GLFW_MOD_CONTROL | GLFW_MOD_SUPER)) != 0, shift = (ev->mods & GLFW_MOD_SHIFT) != 0;
    // show-hint's keymap while the list is open
    if (g_sw.hint_open) {
        const int rows = hint_rows();
        int sel = g_sw.hint_sel;
        switch (key) {
        case 38: sel = sel > 0 ? sel - 1 : g_sw.nhint - 1; break;                // Up (wraps)
        case 40: sel = sel + 1 < g_sw.nhint ? sel + 1 : 0; break;                // Down
        case 33: sel = sel - rows + 1 > 0 ? sel - rows + 1 : 0; break;           // PageUp
        case 34: sel = sel + rows - 1 < g_sw.nhint - 1 ? sel + rows - 1 : g_sw.nhint - 1; break;
        case 36: sel = 0; break;                                                  // Home
        case 35: sel = g_sw.nhint - 1; break;                                     // End
        case 13: case 9: hint_pick(g_sw.hint_sel); return 0;                     // Enter / Tab
        case 27: hint_close(); return 0;                                          // Esc
        default: sel = -1; break;
        }
        if (sel >= 0) {
            g_sw.hint_sel = sel;
            hint_render();
            return 0;
        }
    }
    if (ctrl && key == 83) { // Ctrl+S: opener.KE.updateScript()
        update_script(0);
        return 0;
    }
    if (ctrl && key == 32) { // Ctrl-Space: the hint
        hint_update(side);
        return 0;
    }
    if (ctrl && (key == 191 || key == 111)) { // Ctrl-/: toggleComment
        char *v = get_value(g_sw.ta[side]);
        int a = 0, b = 0, ra, rb;
        html_dom_text_selection(g_sw.ta[side], &a, &b);
        char *r = v ? ed_ks_toggle_comment(v, strlen(v), a, b, &ra, &rb) : NULL;
        if (r) {
            edit_replace(side, ra, rb, r);
            html_dom_text_set_selection(g_sw.ta[side], ra, ra + (int) strlen(r));
            after_edit(side);
        }
        free(r);
        free(v);
        return 0;
    }
    if (ctrl || (ev->mods & GLFW_MOD_ALT)) return 1;
    if (key != 13 && key != 9 && key != 8) return 1;
    char *v = get_value(g_sw.ta[side]);
    if (!v) return 1;
    int a = 0, b = 0;
    html_dom_text_selection(g_sw.ta[side], &a, &b);
    int handled = 1;
    if (key == 13) newline_indent(side, v, a, b);
    else if (key == 9) {
        if (shift) indent_lines(side, v, a, b, 1);       // Shift-Tab: indentAuto
        else if (a != b) indent_lines(side, v, a, b, 0); // defaultTab: indentSelection("add")
        else edit_replace(side, a, b, "\t");             // insertTab
    } else if (a == b && a > 0 && ed_ks_closer(v[a - 1]) && v[a] == ed_ks_closer(v[a - 1])) {
        edit_replace(side, a - 1, a + 1, ""); // closebrackets: Backspace deletes the pair
    } else {
        handled = 0;
    }
    free(v);
    if (!handled) return 1;
    after_edit(side);
    return 0;
}

// ---------------------------------------------------------------------------
// handlers

static int arg_num(const HtmlArg *a, const int n, const int i, const int def) {
    return n > i && (a[i].kind == HTML_ARG_NUM || a[i].kind == HTML_ARG_BOOL) ? (int) a[i].num : def;
}

static int h_tab(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u; (void) ev;
    change_tab(arg_num(a, n, 0, 0));
    return 1;
}

static int h_close(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u; (void) ev; (void) a; (void) n;
    ed_script_close();
    return 1;
}

static int h_update(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u; (void) ev;
    update_script(n > 0 && a[0].kind == HTML_ARG_BOOL && a[0].num != 0);
    return 1;
}

static int h_input(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u; (void) ev;
    on_input(arg_num(a, n, 0, 0) ? 1 : 0);
    return 1;
}

static int h_key(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u;
    return on_key(arg_num(a, n, 0, 0) ? 1 : 0, ev);
}

static int h_blur(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u; (void) ev; (void) a; (void) n;
    hint_close(); // show-hint closes on blur
    return 1;
}

static int h_hint_pick(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u; (void) ev;
    focus_keep(g_sw.tab);
    hint_pick(arg_num(a, n, 0, -1));
    return 0;
}

static void go_to(const int line, const int col) {
    const int side = g_sw.tab;
    char *v = get_value(g_sw.ta[side]);
    if (!v) return;
    int p = 0;
    for (int l = 1; l < line && v[p]; p++) l += v[p] == '\n';
    for (int c = 1; c < col && v[p] && v[p] != '\n'; c++) { // columns are code points
        p++;
        while (((unsigned char) v[p] & 0xC0) == 0x80) p++;
    }
    free(v);
    html_dom_focus(g_sw.ta[side]);
    html_dom_text_set_selection(g_sw.ta[side], p, p);
    g_sw.seen_a[side] = g_sw.seen_b[side] = p; // the input diff's "before" (the frame poll's job)
}

static int h_goto(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u; (void) ev;
    go_to(arg_num(a, n, 0, 1), arg_num(a, n, 1, 1));
    return 1;
}

void ed_script_install(HtmlHandlerEntry *table, size_t *count, const size_t max) {
    const HtmlHandlerEntry entries[] = {
        {"KE.kn.scriptTab", h_tab},       {"KE.kn.scriptClose", h_close},       {"KE.updateScript", h_update},
        {"KE.kn.scriptInput", h_input},   {"KE.kn.scriptKey", h_key},           {"KE.kn.scriptBlur", h_blur},
        {"KE.kn.scriptHintPick", h_hint_pick}, {"KE.kn.scriptGoto", h_goto},
    };
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]) && *count < max; i++) table[(*count)++] = entries[i];
}

// ---------------------------------------------------------------------------
// window

// Editor.scriptPopup (key 0, Scripting menu): open, or focus when open
void editor_script_popup(void) {
    if (g_sw.open) {
        focus_keep(g_sw.tab);
        return;
    }
    build();
    if (!g_sw.win) return;
    g_sw.open = 1;
    g_sw.font = 0;
    memset(g_sw.changed, 0, sizeof(g_sw.changed));
    apply_font();
    load_project();
    g_sw.tab = -1;
    show_tab(0); // KE.currentKey = "client"
    for (int i = 0; i < 2; i++) update_tab_icons(i);
    set_compile("Validate Script", NULL);
    html_dom_set_display(g_sw.win, "");
    focus_keep(0);
    int a = 0, b = 0;
    html_dom_text_selection(g_sw.ta[0], &a, &b);
    g_sw.poll_a = a;
    g_sw.poll_b = b;
}

// the window closes: beforeunload -> KE.updateScript(true)
void ed_script_close(void) {
    if (!g_sw.open) return;
    hint_close();
    update_script(1);
    flush_pending();
    g_sw.open = 0;
    html_dom_blur(g_sw.ta[0]);
    html_dom_blur(g_sw.ta[1]);
    html_dom_set_display(g_sw.win, "none");
    if (g_sw.compile_timer) html_timer_cancel(g_sw.compile_timer);
    g_sw.compile_timer = 0;
}

void ed_script_flush(void) {
    if (g_sw.open) flush_pending();
}

// Escape while the hint list is open (the page's keydown listener sees it first on the web)
int ed_script_escape(void) {
    if (!g_sw.hint_open) return 0;
    hint_close();
    return 1;
}

int ed_script_focused(void) {
    const HtmlElem f = html_dom_active_element();
    return g_sw.open && f && (f == g_sw.ta[0] || f == g_sw.ta[1]);
}

void ed_script_tick(void) {
    if (!g_sw.open) return;
    // the shown project changed (tab switch, close): keep the edits, show the new one's scripts
    if (g_sw.proj != g_ed.proj && !ed_project_background()) {
        flush_pending();
        hint_close();
        load_project();
        memset(g_sw.validated, 0, sizeof(g_sw.validated));
        memset(g_sw.errored, 0, sizeof(g_sw.errored));
        memset(g_sw.changed, 0, sizeof(g_sw.changed));
        for (int i = 0; i < 2; i++) update_tab_icons(i);
    } else if (g_sw.proj && g_sw.proj->revision != g_sw.proj_rev && !g_sw.pending[0] && !g_sw.pending[1]) {
        // something else changed the map (undo, import, the bus): reload a script that differs
        g_sw.proj_rev = g_sw.proj->revision;
        for (int i = 0; i < 2; i++) {
            char *v = get_value(g_sw.ta[i]);
            const char *code = ed_ks_script_text(g_sw.proj->scripts, i);
            if (v && strcmp(v, code)) {
                int a = 0, b = 0;
                html_dom_text_selection(g_sw.ta[i], &a, &b);
                g_sw.in_edit++;
                html_dom_set_value(g_sw.ta[i], code);
                html_dom_text_set_selection(g_sw.ta[i], a, a);
                g_sw.in_edit--;
                set_seen(i);
                run_check(i);
            }
            free(v);
        }
    }
    if ((g_sw.pending[0] || g_sw.pending[1]) && g_ed.now >= g_sw.store_at) flush_pending();
    apply_font();
    // cursorActivity: the hint and the bracket marks follow the caret
    const int side = g_sw.tab;
    int a = 0, b = 0;
    html_dom_text_selection(g_sw.ta[side], &a, &b);
    if (a != g_sw.poll_a || b != g_sw.poll_b) {
        g_sw.poll_a = a;
        g_sw.poll_b = b;
        g_sw.seen_a[side] = a;
        g_sw.seen_b[side] = b;
        decorate(side);
        hint_update(side);
    }
}

void ed_script_reset(void) {
    for (int i = 0; i < 2; i++) free(g_sw.seen[i]);
    free(g_sw.hint);
    if (g_sw.compile_timer) html_timer_cancel(g_sw.compile_timer);
    if (g_sw.saved_timer) html_timer_cancel(g_sw.saved_timer);
    memset(&g_sw, 0, sizeof(g_sw));
}

// ---------------------------------------------------------------------------
// test play (Editor.testMap, E:2183-2215): the server script never runs in Test; the client
// script is compiled first and an error aborts (io_ksT alert). Host keeps both.

int ed_script_prepare_test(cJSON *map, const int host) {
    if (g_sw.open) flush_pending();
    if (host || !map) return 1;
    cJSON *scripts = cJSON_GetObjectItemCaseSensitive(map, "scripts");
    const char *client = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(scripts, "client"));
    if (!client || !client[0]) {
        cJSON_DeleteItemFromObjectCaseSensitive(map, "scripts"); // delete v9140.scripts
        return 1;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(scripts, "server");
    size_t len = 0;
    char *src = ks_decode_script(client, strlen(client), &len);
    if (!src) return 1;
    KsDiag err;
    memset(&err, 0, sizeof(err));
    if (ks_is_compiled_js(src, len)) {
        // the game runs it through the importer (doc/v7/krunkscript.md WP7): test what it runs
        size_t ilen = 0;
        char *imported = ks_import_js(src, len, KS_SIDE_CLIENT, &ilen, &err);
        free(src);
        if (!imported) {
            char msg[320];
            ks_diag_format(&err, msg, sizeof(msg));
            char line[400];
            snprintf(line, sizeof(line), "Client Script: %s: %s", KS_COMPILED_JS_MESSAGE, msg);
            editor_message(line);
            return 0;
        }
        KsProgram *prog = ks_compile(imported, ilen, KS_SIDE_CLIENT, &err, NULL, 0, NULL);
        free(imported);
        if (prog) {
            ks_program_free(prog);
            return 1;
        }
        char msg[320];
        ks_diag_format(&err, msg, sizeof(msg));
        char line[400];
        snprintf(line, sizeof(line), "Client Script (imported from compiled JavaScript): %s", msg);
        editor_message(line);
        return 0;
    }
    KsProgram *prog = ks_compile(src, len, KS_SIDE_CLIENT, &err, NULL, 0, NULL);
    free(src);
    if (prog) {
        ks_program_free(prog);
        return 1;
    }
    char msg[320];
    ks_diag_format(&err, msg, sizeof(msg));
    char line[400];
    snprintf(line, sizeof(line), "Client Script: %s", msg);
    editor_message(line); // io_ksT: alert(error), the test does not start
    if (!g_sw.open) editor_script_popup();
    if (g_sw.open) {
        if (g_sw.tab != 0) change_tab(0);
        run_check(0);
        if (err.line > 0) go_to(err.line, err.col);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// KRUNKNATIVE_EDITOR_SCRIPT "script:..." (editor.c) and "dump:script:FILE": open, close,
// tab:client|server, validate, save, caret:LINE:COL, load:FILE (the tab's text, as a paste),
// type:TEXT (typed; \n = Enter, \t = Tab), insert:TEXT (raw, at the caret), select:A:B (byte
// offsets), key:[ctrl+][shift+]NAME (the keydown handler), hint:I (pick hint row I), flush (keep the edits
// in the project now), text:FILE (the tab's text)

void ed_script_command(const char *arg) {
    if (!arg) return;
    if (!strcmp(arg, "open")) editor_script_popup();
    else if (!strcmp(arg, "close")) ed_script_close();
    else if (!g_sw.open) return;
    else if (!strncmp(arg, "tab:", 4)) change_tab(!strcmp(arg + 4, "server") || !strcmp(arg + 4, "1"));
    else if (!strcmp(arg, "validate")) update_script(0);
    else if (!strcmp(arg, "save")) update_script(1);
    else if (!strncmp(arg, "caret:", 6)) {
        int line = 1, col = 1;
        sscanf(arg + 6, "%d:%d", &line, &col);
        go_to(line, col);
    } else if (!strncmp(arg, "load:", 5)) {
        // the tab's whole text from a file, as a paste
        FILE *f = fopen(arg + 5, "rb");
        if (!f) return;
        fseek(f, 0, SEEK_END);
        const long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = malloc((size_t) (n > 0 ? n : 0) + 1);
        if (buf && fread(buf, 1, (size_t) n, f) == (size_t) n) {
            buf[n] = 0;
            char *v = get_value(g_sw.ta[g_sw.tab]);
            edit_replace(g_sw.tab, 0, v ? (int) strlen(v) : 0, buf);
            html_dom_text_set_selection(g_sw.ta[g_sw.tab], 0, 0);
            free(v);
            after_edit(g_sw.tab);
        }
        free(buf);
        fclose(f);
    } else if (!strncmp(arg, "type:", 5)) {
        // typed text (closebrackets applies to each character); "\n" = Enter, "\t" = Tab
        const int side = g_sw.tab;
        focus_keep(side);
        for (const char *p = arg + 5; *p; p++) {
            if (*p == '\\' && (p[1] == 'n' || p[1] == 't')) {
                HtmlEvent ev = {.type = "keydown"};
#ifdef __EMSCRIPTEN__
                ev.key = p[1] == 'n' ? 13 : 9;
#else
                ev.key = p[1] == 'n' ? GLFW_KEY_ENTER : GLFW_KEY_TAB;
#endif
                on_key(side, &ev);
                p++;
                continue;
            }
            int a = 0, b = 0;
            html_dom_text_selection(g_sw.ta[side], &a, &b);
            const char s[2] = {*p, 0};
            g_sw.in_edit++;
            html_dom_text_replace(g_sw.ta[side], a, b, s);
            g_sw.in_edit--;
            on_input(side);
        }
    } else if (!strncmp(arg, "insert:", 7)) {
        // raw text at the caret (CodeMirror replaceRange: no closebrackets / indentation);
        // \n and \t escapes
        const int side = g_sw.tab;
        char *t = malloc(strlen(arg + 7) + 1);
        size_t k = 0;
        for (const char *p = arg + 7; t && *p; p++) {
            if (*p == '\\' && (p[1] == 'n' || p[1] == 't')) {
                t[k++] = p[1] == 'n' ? '\n' : '\t';
                p++;
            } else {
                t[k++] = *p;
            }
        }
        if (t) {
            t[k] = 0;
            focus_keep(side);
            int a = 0, b = 0;
            html_dom_text_selection(g_sw.ta[side], &a, &b);
            edit_replace(side, a, b, t);
            after_edit(side);
        }
        free(t);
    } else if (!strncmp(arg, "hint:", 5)) {
        hint_pick(atoi(arg + 5));
    } else if (!strncmp(arg, "select:", 7)) {
        int a = 0, b = 0;
        sscanf(arg + 7, "%d:%d", &a, &b);
        focus_keep(g_sw.tab);
        html_dom_text_set_selection(g_sw.ta[g_sw.tab], a, b);
        html_dom_text_selection(g_sw.ta[g_sw.tab], &g_sw.seen_a[g_sw.tab], &g_sw.seen_b[g_sw.tab]);
    } else if (!strncmp(arg, "key:", 4)) {
        // a key through the textarea's keydown handler: [ctrl+][shift+]tab|enter|backspace|up|
        // down|escape|space|s|/ (what the handler does; the widget's own default is not run)
        const char *k = arg + 4;
        HtmlEvent ev = {.type = "keydown"};
        for (;;) {
            if (!strncmp(k, "ctrl+", 5)) ev.mods |= GLFW_MOD_CONTROL, k += 5;
            else if (!strncmp(k, "shift+", 6)) ev.mods |= GLFW_MOD_SHIFT, k += 6;
            else break;
        }
        static const struct { const char *name; int glfw, dom; } keys[] = {
            {"tab", GLFW_KEY_TAB, 9}, {"enter", GLFW_KEY_ENTER, 13}, {"backspace", GLFW_KEY_BACKSPACE, 8}, {"up", GLFW_KEY_UP, 38},
            {"down", GLFW_KEY_DOWN, 40}, {"escape", GLFW_KEY_ESCAPE, 27}, {"space", GLFW_KEY_SPACE, 32}, {"s", GLFW_KEY_S, 83},
            {"/", GLFW_KEY_SLASH, 191},
        };
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            if (strcmp(k, keys[i].name)) continue;
#ifdef __EMSCRIPTEN__
            ev.key = keys[i].dom;
#else
            ev.key = keys[i].glfw;
#endif
            focus_keep(g_sw.tab);
            on_key(g_sw.tab, &ev);
        }
    } else if (!strcmp(arg, "flush")) {
        flush_pending();
    } else if (!strncmp(arg, "text:", 5)) {
        // the shown tab's text into a file
        char *v = get_value(g_sw.ta[g_sw.tab]);
        FILE *f = fopen(arg + 5, "wb");
        if (f && v) fputs(v, f);
        if (f) fclose(f);
        free(v);
    }
}

// The window as text (doc/ref/krunker-v7.1.4/editor/scripting.dump.txt has v7's, written by
// tools/editor/v7ref.js from the recovered page): H (the label), T per tab (" *" = selected,
// [E] / [V] / [C] = the tab icon), B (the status button), C (the shown tab: lines, font), E per
// error-marked line, D per listed diagnostic (KrunkNative's list), K per hint ("* " = active),
// S the selection.
int ed_script_dump(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    if (!g_sw.open) {
        fputs("closed\n", f);
        fclose(f);
        return 1;
    }
    fputs("H KRUNKSCRIPT\n", f);
    for (int i = 0; i < 2; i++) {
        fprintf(f, "T %s%s%s%s%s\n", i ? "Server" : "Client", i == g_sw.tab ? " *" : "", g_sw.errored[i] ? " [E]" : "", g_sw.validated[i] ? " [V]" : "",
                g_sw.changed[i] ? " [C]" : "");
    }
    char t[128] = "";
    html_dom_get_text(g_sw.compile, t, sizeof(t));
    fprintf(f, "B %s\n", t);
    char *v = get_value(g_sw.ta[g_sw.tab]);
    int lines = 1;
    for (const char *p = v; p && *p; p++) lines += *p == '\n';
    fprintf(f, "C %s lines=%d font=%dpx\n", side_name(g_sw.tab), lines, g_sw.font);
    for (int i = 0; i < g_sw.ndiags[g_sw.tab]; i++) {
        if (g_sw.diags[g_sw.tab][i].error) fprintf(f, "E %d\n", g_sw.diags[g_sw.tab][i].line > 0 ? g_sw.diags[g_sw.tab][i].line : 1);
    }
    for (int i = 0; i < g_sw.ndiags[g_sw.tab]; i++) {
        const EdKsDiag *d = &g_sw.diags[g_sw.tab][i];
        fprintf(f, "D %s %d:%d %s\n", d->error ? "error" : "warning", d->line, d->col, d->message);
    }
    for (int i = 0; g_sw.hint_open && i < g_sw.nhint; i++) fprintf(f, "K %s%s | %s\n", i == g_sw.hint_sel ? "* " : "", g_sw.hint[i].text, g_sw.hint[i].type);
    int a = 0, b = 0;
    html_dom_text_selection(g_sw.ta[g_sw.tab], &a, &b);
    fprintf(f, "S %d %d\n", a, b);
    free(v);
    fclose(f);
    return 1;
}
