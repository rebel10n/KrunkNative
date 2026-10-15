/*
 * test_css_media.c - @media (css_sheet.c, Media Queries 4): the query
 * evaluator against a set environment, and the cascade of rules inside
 * @media blocks across viewport changes (css_sheet_media_update), including
 * the 11 @media rules of Krunker v10.0.2's ui/css/svelte.css at 1280 and
 * 1000 px.  Expected results are Chrome's (window.matchMedia).
 *
 * Throwaway build (a CMakeLists.txt in a scratch directory; R = the repository):
 *
 *   cmake_minimum_required(VERSION 3.16)
 *   project(kn_media_test C)
 *   set(CMAKE_C_STANDARD 11)
 *   set(R /path/to/KrunkNative)
 *   set(E ${R}/client/src/html/engine)
 *   add_subdirectory(${R}/client/lib/lexbor lexbor)
 *   add_executable(test_css_media ${E}/tests/test_css_media.c ${E}/dom.c ${E}/css_sheet.c
 *                  ${E}/css_cascade.c ${E}/css_values.c ${E}/util.c)
 *   target_include_directories(test_css_media PRIVATE ${E} ${R}/client/include ${R}/client/lib/lexbor)
 *   target_compile_definitions(test_css_media PRIVATE KN_REPO_ROOT="${R}")
 *   target_link_libraries(test_css_media lexbor m)
 *
 * Usage: test_css_media [repo root]. Exit code: number of failed checks.
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

static int g_fail, g_pass;

static void env(float w, float h)
{
    CssMediaEnv e = { w, h, 1.0f, 0, 0, 0 };
    css_media_set_env(&e);
}

static void mq(const char *q, int expected)
{
    const int got = css_media_query_matches(q, strlen(q));
    if (got == expected) {
        g_pass++;
        return;
    }
    g_fail++;
    printf("FAIL @media %s at %.0fx%.0f: got %d expected %d\n", q, (double)css_media_env()->width,
           (double)css_media_env()->height, got, expected);
}

static void evaluator(void)
{
    env(1280, 720);
    mq("", 1);
    mq("all", 1);
    mq("screen", 1);
    mq("print", 0);
    mq("not print", 1);
    mq("only screen", 1);
    mq("speech, screen", 1);
    mq("(max-width: 1200px)", 0);
    mq("(max-width: 1280px)", 1);
    mq("(min-width: 1280px)", 1);
    mq("(min-width: 1281px)", 0);
    mq("screen and (min-width: 768px) and (max-width: 1300px)", 1);
    mq("screen and (max-width: 600px)", 0);
    mq("not screen and (max-width: 600px)", 1);
    mq("(orientation: landscape)", 1);
    mq("(orientation: portrait)", 0);
    mq("(width >= 1280px)", 1);
    mq("(width > 1280px)", 0);
    mq("(1000px < width <= 1280px)", 1);
    mq("(1000px < width < 1280px)", 0);
    mq("(400px <= width)", 1);
    mq("(max-width: 50em)", 0);
    mq("(max-width: 80em)", 1);
    mq("(min-aspect-ratio: 16/9)", 1);
    mq("(min-aspect-ratio: 16/8)", 0);
    mq("(aspect-ratio: 16/9)", 1);
    mq("(prefers-color-scheme: dark)", 0);
    mq("(prefers-color-scheme: light)", 1);
    mq("(prefers-reduced-motion: reduce)", 0);
    mq("(prefers-reduced-motion)", 0);
    mq("(hover: hover) and (pointer: fine)", 1);
    mq("(hover)", 1);
    mq("(pointer: coarse)", 0);
    mq("(any-pointer: coarse), (max-width: 400px)", 0);
    mq("(-webkit-min-device-pixel-ratio: 2)", 0);
    mq("(min-resolution: 1dppx)", 1);
    mq("(min-resolution: 2dppx)", 0);
    mq("(color)", 1);
    mq("(monochrome)", 0);
    mq("not (max-width: 600px)", 1);
    mq("(max-width: 600px) or (min-width: 1000px)", 1);
    mq("((max-width: 600px) or (min-width: 1000px)) and (orientation: landscape)", 1);
    mq("(max-width: 600px) and (min-width: 100px) or (color)", 0);  /* mixed and/or: invalid */
    mq("(min-width)", 0);                                            /* min- needs a value */
    mq("(unknown-feature: 3)", 0);
    mq("not (unknown-feature: 3)", 0);                               /* unknown stays unknown */
    mq("(unknown-feature: 3), screen", 1);
    mq("(max-width: 600)", 0);                                       /* unitless length: invalid */
    mq("(min-width: 0)", 1);
    mq("garbage here", 0);
    mq("screen and", 0);
    env(1000, 800);
    mq("(max-width: 1024px)", 1);
    mq("(max-width: 1100px)", 1);
    mq("(max-width: 1200px)", 1);
    mq("(max-width: 900px)", 0);
    mq("(max-width: 768px)", 0);
    env(600, 900);
    mq("(orientation: portrait)", 1);
    mq("(max-width: 600px)", 1);
    mq("(max-width: 37.5em)", 1);
    mq("(max-width: 37.4em)", 0);
}

/* FRVR Krunker Strike 1.37.2's page: its whole @media list (tools/strike/strike_data.json
 * media_queries), at a 16:9 desktop, a wide short window and a portrait phone (Chrome). */
static void strike(void)
{
    static const char *q[7] = {
        "(hover: hover) and (pointer: fine)", "(max-aspect-ratio: 16/9)", "(max-height: 400px)",
        "only screen and (min-width: 1200px)", "only screen and (min-width: 640px)", "screen and (max-height: 420px)",
        "screen and (min-width: 320px) and (max-width: 767px) and (orientation: portrait)",
    };
    static const struct { float w, h; int want[7]; } at[] = {
        { 1280, 720, { 1, 1, 0, 1, 1, 0, 0 } },   /* exactly 16/9: max-aspect-ratio is inclusive */
        { 1920, 1080, { 1, 1, 0, 1, 1, 0, 0 } },
        { 1366, 600, { 1, 0, 0, 1, 1, 0, 0 } },   /* wider than 16:9 */
        { 840, 410, { 1, 0, 0, 0, 1, 1, 0 } },
        { 375, 667, { 1, 1, 0, 0, 0, 0, 1 } },    /* portrait phone viewport */
    };
    for (size_t i = 0; i < sizeof(at) / sizeof(at[0]); i++) {
        env(at[i].w, at[i].h);
        for (int j = 0; j < 7; j++) mq(q[j], at[i].want[j]);
    }
    env(1280, 720);
    mq("(max-aspect-ratio: 1.7)", 0);                                /* a <number> ratio */
    mq("(min-aspect-ratio: 1777/1000)", 1);
    mq("(max-aspect-ratio: 16 / 9) and (min-aspect-ratio: 16/9)", 1);
}

static char *read_file(const char *root, const char *rel, size_t *len)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", root, rel);
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

static void check_int(const char *what, long got, long expected)
{
    if (got == expected) {
        g_pass++;
        return;
    }
    g_fail++;
    printf("FAIL %s: got %ld expected %ld\n", what, got, expected);
}

static const CssComputed *style_of(Dom *dom, const char *sel)
{
    const HtmlElem e = dom_query(dom, 0, sel);
    return e ? cascade_elem_computed(dom, e) : NULL;
}

/* rules inside @media flip with the viewport; order and specificity hold */
static void cascade(const char *root)
{
    static const char html[] =
        "<html><body><div id=a class=\"guide-glossary svelte-4log2q\"></div><div id=b class=x></div>"
        "<div id=c class=\"kid-display-content svelte-lbi007\"></div></body></html>";
    static const char css[] =
        ".x { width: 10px }\n"
        "@media (max-width: 1100px) { .x { width: 20px } @media (orientation: portrait) { .x { width: 30px } } }\n"
        ".x { height: 5px }\n"
        "@media print { .x { height: 99px } }\n";
    Dom *dom = dom_create();
    dom_load_html(dom, html, strlen(html));
    CssSheet *ua = css_sheet_load(KN_UA_CSS, strlen(KN_UA_CSS), "ua.css");
    size_t len;
    char *svelte = read_file(root, "ui/css/svelte.css", &len);
    CssSheet *sheets[2] = { css_sheet_load(svelte, len, "css/svelte.css"), css_sheet_load(css, strlen(css), "t.css") };
    free(svelte);
    check_int("svelte.css @media blocks", (long)css_sheet_media_count(sheets[0]), 11);

    env(1280, 720);
    for (int i = 0; i < 2; i++) css_sheet_media_update(sheets[i]);
    cascade_restyle(dom, ua, sheets, 2);
    check_int("1280: svelte.css matching @media", (long)css_sheet_media_matching(sheets[0]), 0);
    const CssComputed *c = style_of(dom, "#b");
    check_int("1280: .x width", c ? (long)c->width.px : -1, 10);
    check_int("1280: .x height (print ignored)", c ? (long)c->height.px : -1, 5);
    c = style_of(dom, "#a");
    check_int("1280: .guide-glossary columns", c ? (long)c->grid_column_count : -1, 3);
    c = style_of(dom, "#c");
    check_int("1280: .kid-display-content column", c ? (long)c->flex_direction : -1, CSS_FLEX_DIRECTION_ROW);

    env(1000, 720);
    int changed = 0;
    for (int i = 0; i < 2; i++) changed |= css_sheet_media_update(sheets[i]);
    check_int("1000: media changed", changed, 1);
    check_int("1000: svelte.css matching @media", (long)css_sheet_media_matching(sheets[0]), 3);
    dom_mark_all_dirty(dom);
    cascade_restyle(dom, ua, sheets, 2);
    c = style_of(dom, "#b");
    check_int("1000: .x width", c ? (long)c->width.px : -1, 20);
    c = style_of(dom, "#a");
    check_int("1000: .guide-glossary columns (max-width 1100px)", c ? (long)c->grid_column_count : -1, 2);
    c = style_of(dom, "#c");
    check_int("1000: .kid-display-content column (max-width 1024px)", c ? (long)c->flex_direction : -1,
              CSS_FLEX_DIRECTION_COLUMN);

    env(800, 1000);
    changed = 0;
    for (int i = 0; i < 2; i++) changed |= css_sheet_media_update(sheets[i]);
    check_int("800x1000: media changed", changed, 1);
    dom_mark_all_dirty(dom);
    cascade_restyle(dom, ua, sheets, 2);
    c = style_of(dom, "#b");
    check_int("800x1000: nested portrait .x width", c ? (long)c->width.px : -1, 30);

    env(800, 1001);
    changed = 0;
    for (int i = 0; i < 2; i++) changed |= css_sheet_media_update(sheets[i]);
    check_int("800x1001: nothing flips", changed, 0);

    env(1280, 720);
    for (int i = 0; i < 2; i++) css_sheet_media_update(sheets[i]);
    dom_mark_all_dirty(dom);
    cascade_restyle(dom, ua, sheets, 2);
    c = style_of(dom, "#b");
    check_int("back to 1280: .x width", c ? (long)c->width.px : -1, 10);

    for (int i = 0; i < 2; i++) css_sheet_destroy(sheets[i]);
    css_sheet_destroy(ua);
    dom_destroy(dom);
}

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : KN_REPO_ROOT;
    evaluator();
    strike();
    cascade(root);
    printf("test_css_media: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
