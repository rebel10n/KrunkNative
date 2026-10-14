/*
 * test_runtime_sheet.c - a stylesheet added after the page was styled
 * (html_ui_add_stylesheet -> html_ui_backend_add_stylesheet in engine.c): the
 * page (ui/index.html, Krunker v10.0.2) is loaded with its <link> sheets and its <style>
 * block and restyled, the developer console's markup (client/src/ui/ui_console.c,
 * from Krunker v3.1.1 mod/devConsole.ts) is appended to <body>, then
 * ui/css/devconsole.css is appended as the last sheet and the whole document
 * marked dirty exactly like the engine does. Checks that
 * the new rules apply (and win ties as the last sheet), that class toggles
 * (.hints.isOpen) keep working, and that the sheet's @font-face is visible.
 *
 * Throwaway build (a CMakeLists.txt in a scratch directory; R = the repository):
 *
 *   cmake_minimum_required(VERSION 3.16)
 *   project(kn_sheet_test C)
 *   set(CMAKE_C_STANDARD 11)
 *   set(R /path/to/KrunkNative)
 *   set(E ${R}/client/src/html/engine)
 *   add_subdirectory(${R}/client/lib/lexbor lexbor)
 *   add_executable(test_runtime_sheet ${E}/tests/test_runtime_sheet.c ${E}/dom.c ${E}/css_sheet.c
 *                  ${E}/css_cascade.c ${E}/css_values.c ${E}/util.c)
 *   target_include_directories(test_runtime_sheet PRIVATE ${E} ${R}/client/include ${R}/client/lib/lexbor)
 *   target_compile_definitions(test_runtime_sheet PRIVATE KN_REPO_ROOT="${R}")
 *   target_link_libraries(test_runtime_sheet lexbor m)
 *
 * Second part (regression): the v10 page (ui/index.html) with every <link> / <style> sheet it has
 * plus enough empty sheets for 20 author sheets, then devconsole.css and a page-module sheet (like
 * html_ui_add_stylesheet("css/kn-v10-news.css")) appended at runtime. The cascade's sheet index was a
 * 4-bit field (UA + 14 author sheets): every sheet past the 14th was silently dropped, so one extra
 * <link> left the dev console unstyled and the news popup collapsed.
 *
 * Usage: test_runtime_sheet [repo root]. Exit code: number of failed checks.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "css_cascade.h"
#include "css_sheet.h"
#include "css_values.h"
#include "dom.h"
#include "ua_css.h"

#ifndef KN_REPO_ROOT
#define KN_REPO_ROOT "."
#endif

static int g_fail;
static int g_pass;
static const char *g_root = KN_REPO_ROOT;
static Dom *g_dom;
static CssSheet *g_ua;
static CssSheet *g_sheets[40];
static int g_n_sheets;

static const char *const k_display[] = {"inline", "block", "none", "inline-block", "flex", "inline-flex", "grid",
                                        "inline-grid", "table", "inline-table", "table-row", "table-cell",
                                        "table-row-group", "list-item", "contents"};
static const char *const k_position[] = {"static", "relative", "absolute", "fixed", "sticky"};

/* the overlay ui_console.c builds (devConsole.ts DOM, abridged to the styled parts) */
static const char k_console_html[] =
    "<div id=\"devConsoleOverlay\"><div class=\"modal\">"
    "<div class=\"titlebar\"><span>Console</span><span class=\"x\">x</span></div>"
    "<div class=\"body\"><div class=\"logs\"></div>"
    "<div class=\"inputShell\"><div class=\"hints\"></div>"
    "<form class=\"row\"><input type=\"text\"><button type=\"submit\">Submit</button></form></div></div>"
    "<div class=\"grip\"></div></div></div>";

static char *read_file(const char *rel, size_t *len)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", g_root, rel);
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(99);
    }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) exit(99);
    buf[n] = '\0';
    fclose(f);
    if (len) *len = (size_t)n;
    return buf;
}

static void check_str(const char *what, const char *got, const char *expected)
{
    if (got && !strcmp(got, expected)) {
        g_pass++;
        return;
    }
    g_fail++;
    printf("FAIL %s: got '%s' expected '%s'\n", what, got ? got : "(null)", expected);
}

static void fmt(const CssComputed *c, const char *prop, char *out, size_t cap)
{
    out[0] = '\0';
    if (!c) {
        snprintf(out, cap, "(no computed)");
    } else if (!strcmp(prop, "display")) {
        snprintf(out, cap, "%s", k_display[c->display]);
    } else if (!strcmp(prop, "position")) {
        snprintf(out, cap, "%s", k_position[c->position]);
    } else if (!strcmp(prop, "background-color")) {
        snprintf(out, cap, "rgb(%d, %d, %d)", c->background_color.r, c->background_color.g, c->background_color.b);
    } else if (!strcmp(prop, "color")) {
        snprintf(out, cap, "rgb(%d, %d, %d)", c->color.r, c->color.g, c->color.b);
    } else if (!strcmp(prop, "font-family")) {
        snprintf(out, cap, "%s", c->font_family_count ? c->font_families[0] : "");
    } else if (!strcmp(prop, "font-size")) {
        snprintf(out, cap, "%gpx", (double)c->font_size);
    }
}

static void check(const char *selector, const char *prop, const char *expected)
{
    const HtmlElem e = dom_query(g_dom, 0, selector);
    char buf[256], what[256];
    snprintf(what, sizeof(what), "%s { %s }", selector, prop);
    if (!e) {
        check_str(what, "(no element)", expected);
        return;
    }
    fmt(cascade_elem_computed(g_dom, e), prop, buf, sizeof(buf));
    check_str(what, buf, expected);
}

static void restyle(void)
{
    cascade_restyle(g_dom, g_ua, g_sheets, g_n_sheets);
}

/* ui/index.html's <link rel="stylesheet"> sheets in order (relative to ui/: the url() bases) */
static void load_page_sheets(const char *html)
{
    for (const char *p = html; (p = strstr(p, "<link rel=\"stylesheet\" href=\"")) != NULL;) {
        p += strlen("<link rel=\"stylesheet\" href=\"");
        const char *q = strchr(p, '"');
        if (!q || g_n_sheets >= 30) break;
        char href[128], rel[192];
        snprintf(href, sizeof(href), "%.*s", (int)(q - p), p);
        snprintf(rel, sizeof(rel), "ui/%s", href);
        size_t clen;
        char *css = read_file(rel, &clen);
        g_sheets[g_n_sheets++] = css_sheet_load(css, clen, href);
        free(css);
        p = q;
    }
}

/* the v10 page with 20 author sheets, then two runtime sheets past the old 15-sheet cap */
static void many_sheets(void)
{
    size_t len;
    char *html = read_file("ui/index.html", &len);
    dom_destroy(g_dom);
    g_dom = dom_create();
    if (dom_load_html(g_dom, html, len) != 0) {
        printf("FAIL v10 dom_load_html\n");
        g_fail++;
        free(html);
        return;
    }
    g_n_sheets = 0;
    load_page_sheets(html);
    const char *st = strstr(html, "<style>");
    const char *st_end = st ? strstr(st, "</style>") : NULL;
    if (st_end) g_sheets[g_n_sheets++] = css_sheet_load(st + 7, (size_t)(st_end - st - 7), "index.html");
    free(html);
    while (g_n_sheets < 20) g_sheets[g_n_sheets++] = css_sheet_load("", 0, "pad.css");
    restyle();
    dom_insert_html(g_dom, dom_body(g_dom), DOM_INSERT_BEFORE_END, k_console_html, strlen(k_console_html));
    static const char news_html[] = "<div id=\"knRtNews\" class=\"knRtCard\"></div>";
    dom_insert_html(g_dom, dom_body(g_dom), DOM_INSERT_BEFORE_END, news_html, strlen(news_html));
    char *css = read_file("ui/css/devconsole.css", &len);
    g_sheets[g_n_sheets++] = css_sheet_load(css, len, "css/devconsole.css");
    free(css);
    static const char news_css[] = ".knRtCard{display:flex;position:relative}";
    g_sheets[g_n_sheets++] = css_sheet_load(news_css, strlen(news_css), "css/kn-rt-news.css");
    static const char later_css[] = ".knRtCard{display:grid}"; /* same specificity, later sheet: wins */
    g_sheets[g_n_sheets++] = css_sheet_load(later_css, strlen(later_css), "css/kn-rt-later.css");
    dom_mark_all_dirty(g_dom);
    restyle();
    char n[32];
    snprintf(n, sizeof(n), "%d", g_n_sheets);
    check_str("v10 author sheets", g_n_sheets >= 23 ? "23+" : n, "23+");
    check("#devConsoleOverlay", "display", "none");   /* sheet 21: the runtime console sheet applies */
    check("#devConsoleOverlay", "position", "fixed");
    check("#knRtNews", "position", "relative");        /* sheet 22: a page module's runtime sheet */
    check("#knRtNews", "display", "grid");             /* sheet 23 beats 22's flex on a tie */
}

int main(int argc, char **argv)
{
    if (argc > 1) g_root = argv[1];
    size_t len;
    char *html = read_file("ui/index.html", &len);
    g_dom = dom_create();
    if (dom_load_html(g_dom, html, len) != 0) {
        printf("FAIL dom_load_html\n");
        return 1;
    }
    g_ua = css_sheet_load(KN_UA_CSS, strlen(KN_UA_CSS), "ua.css");
    load_page_sheets(html);
    /* the page's <style> block (the ad rules) */
    const char *st = strstr(html, "<style>");
    const char *st_end = st ? strstr(st, "</style>") : NULL;
    if (st_end) g_sheets[g_n_sheets++] = css_sheet_load(st + 7, (size_t)(st_end - st - 7), "index.html");
    free(html);
    restyle();

    /* the console markup before its sheet: plain UA/author defaults */
    dom_insert_html(g_dom, dom_body(g_dom), DOM_INSERT_BEFORE_END, k_console_html, strlen(k_console_html));
    restyle();
    check("#devConsoleOverlay", "display", "block");
    check("#devConsoleOverlay .modal", "position", "static");

    /* html_ui_backend_add_stylesheet: append, register, mark everything dirty */
    char *css = read_file("ui/css/devconsole.css", &len);
    CssSheet *sheet = css_sheet_load(css, len, "css/devconsole.css");
    free(css);
    if (!sheet) {
        printf("FAIL devconsole.css did not parse\n");
        return 1;
    }
    uint32_t faces = 0;
    const CssFontFace *face = css_sheet_font_faces(sheet, &faces);
    check_str("@font-face count", faces == 1 ? "1" : "other", "1");
    if (faces == 1) {
        check_str("@font-face family", face[0].family, "LuconDevConsole");
        check_str("@font-face src", face[0].src_count ? face[0].src[0] : "", "fonts/lucon.ttf");
    }
    g_sheets[g_n_sheets++] = sheet;
    dom_mark_all_dirty(g_dom);
    restyle();

    check("#devConsoleOverlay", "display", "none");
    check("#devConsoleOverlay", "position", "fixed");

    /* visible: the overlay's inline display (ui_console.c update()) */
    dom_set_style(g_dom, dom_query(g_dom, 0, "#devConsoleOverlay"), "display", "block");
    restyle();
    check("#devConsoleOverlay", "display", "block");
    check("#devConsoleOverlay .modal", "position", "absolute");
    check("#devConsoleOverlay .modal", "display", "flex");
    check("#devConsoleOverlay .modal", "background-color", "rgb(122, 122, 122)");
    check("#devConsoleOverlay .modal", "font-family", "lucondevconsole");   /* #devConsoleOverlay * beats main.css * */
    check("#devConsoleOverlay .logs", "background-color", "rgb(106, 106, 106)");
    check("#devConsoleOverlay .logs", "color", "rgb(224, 224, 224)");
    check("#devConsoleOverlay .logs", "font-size", "12px");
    check("#devConsoleOverlay .row input", "background-color", "rgb(88, 88, 88)");
    check("#devConsoleOverlay .row input", "font-family", "lucondevconsole");
    check("#devConsoleOverlay .titlebar span", "color", "rgb(255, 255, 255)");
    check("#devConsoleOverlay .hints", "display", "none");
    check("#devConsoleOverlay .grip", "position", "absolute");

    /* class toggles after the runtime sheet */
    const HtmlElem hints = dom_query(g_dom, 0, "#devConsoleOverlay .hints");
    dom_set_class(g_dom, hints, "hints isOpen");
    restyle();
    check("#devConsoleOverlay .hints", "display", "block");
    dom_set_class(g_dom, hints, "hints");
    restyle();
    check("#devConsoleOverlay .hints", "display", "none");

    /* the page's own rules are untouched */
    check("#menuWindow", "font-family", "gamefont");

    many_sheets();

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
