// ui_test: command-line driver for the in-house HTML/CSS UI engine
// (client/src/html/engine, built when KRUNKNATIVE_HTML_ENGINE=ON; plan in
// doc/html-engine-plan.md). Every subcommand is a small self-contained check, dump
// or benchmark run from the build directory, e.g.
//
//   ./cmake-build-debug/ui_test help
//   ./cmake-build-debug/ui_test lexbor-probe
//   ./cmake-build-debug/ui_test render <scenario.json> <out.png>   (tools/uidiff)
//
// `make ui-test ARGS="..."` configures with HTML_ENGINE=ON, builds only this target
// and runs it. Exit status: 0 success, 1 failure, 2 usage error.
#include <gl.h>  // before GLFW (no system GL header)
#include <GLFW/glfw3.h>
#include <headless.h>  // --headless / KRUNKNATIVE_HEADLESS=1: EGL device context, no window
#include <html_ui.h>
#include <html_dom.h>
#include <lexbor.h>
#include <png_write.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>
#include "engine/engine.h"
#include "engine/events.h"

// ui_test does not link client/src/stb_image.c: image.c and the background loader need it.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A subcommand receives its own argv: argv[0] is the subcommand name. The return
// value is the process exit status.
typedef int (*UiTestCommand)(int argc, char **argv);

typedef struct {
    const char *name;  // kebab-case, matched exactly against argv[1]
    const char *args;  // usage summary printed after the name ("" when none)
    const char *help;  // one line
    UiTestCommand fn;
} UiTestSubcommand;

// ------------------------------------------------------------------ lexbor-probe
// Smoke test for the vendored Lexbor amalgamation (client/lib/lexbor): HTML5 tree
// construction with sloppy markup, a stylesheet with a pseudo-element selector and a
// calc() value, and selector matching against the parsed document.

static lxb_status_t probe_print(const lxb_char_t *data, const size_t len, void *ctx) {
    (void) ctx;
    printf("%.*s", (int) len, (const char *) data);
    return LXB_STATUS_OK;
}

static lxb_status_t probe_count_match(lxb_dom_node_t *node, const lxb_css_selector_specificity_t spec, void *ctx) {
    (void) node;
    (void) spec;
    (*(unsigned *) ctx)++;
    return LXB_STATUS_OK;
}

static int probe_fail(const char *what, const lxb_status_t status) {
    fprintf(stderr, "lexbor-probe: %s failed (lxb_status %u)\n", what, (unsigned) status);
    return 0;
}

// Number of nodes under `root` matching `selector`, -1 when the selector does not parse.
// The selector lists live in the lxb_css_memory_t the caller attached to `parser`
// (lxb_css_parser_memory_set); lxb_css_selector_list_destroy_memory() would free that
// whole pool, so the lists are released together with the pool by the caller.
static int probe_match_count(lxb_css_parser_t *parser, lxb_selectors_t *selectors, lxb_dom_node_t *root, const char *selector) {
    lxb_css_selector_list_t *list = lxb_css_selectors_parse(parser, (const lxb_char_t *) selector, strlen(selector));
    if (parser->status != LXB_STATUS_OK || !list) return -1;

    unsigned count = 0;
    const lxb_status_t status = lxb_selectors_find(selectors, root, list, probe_count_match, &count);

    return status == LXB_STATUS_OK ? (int) count : -1;
}

static int probe_expect_matches(lxb_css_parser_t *parser, lxb_selectors_t *selectors, lxb_dom_node_t *root, const char *selector, const int expected) {
    const int count = probe_match_count(parser, selectors, root, selector);
    printf("  %-22s -> %d match(es)%s\n", selector, count, count == expected ? "" : "  EXPECTED");
    if (count != expected) fprintf(stderr, "lexbor-probe: selector '%s' matched %d node(s), expected %d\n", selector, count, expected);
    return count == expected;
}

static int cmd_lexbor_probe(const int argc, char **argv) {
    (void) argc;
    (void) argv;

    static const char html[] = "<div id=a class=b>hi</br><input type=undefined></input></div>";
    static const char css[] = ".x::after{content:\"\";display:block}#a{width:calc(100% - 4px)}";

    int ok = 1;
    lxb_status_t status;

    lxb_html_document_t *document = lxb_html_document_create();
    lxb_css_parser_t *selector_parser = lxb_css_parser_create();
    lxb_css_parser_t *sheet_parser = lxb_css_parser_create();
    lxb_selectors_t *selectors = lxb_selectors_create();
    lxb_css_memory_t *selector_memory = lxb_css_memory_create();
    lxb_css_stylesheet_t *sheet = NULL;

    if (!document || !selector_parser || !sheet_parser || !selectors || !selector_memory) ok = probe_fail("object creation", LXB_STATUS_ERROR_MEMORY_ALLOCATION);

    if (ok && (status = lxb_css_memory_init(selector_memory, 128)) != LXB_STATUS_OK) ok = probe_fail("lxb_css_memory_init", status);
    if (ok && (status = lxb_css_parser_init(selector_parser, NULL)) != LXB_STATUS_OK) ok = probe_fail("lxb_css_parser_init", status);
    if (ok) lxb_css_parser_memory_set(selector_parser, selector_memory);
    if (ok && (status = lxb_css_parser_init(sheet_parser, NULL)) != LXB_STATUS_OK) ok = probe_fail("lxb_css_parser_init", status);
    if (ok && (status = lxb_selectors_init(selectors)) != LXB_STATUS_OK) ok = probe_fail("lxb_selectors_init", status);

    // HTML: </br> becomes <br>, </input> is dropped, type=undefined is kept verbatim.
    if (ok) {
        status = lxb_html_document_parse(document, (const lxb_char_t *) html, sizeof(html) - 1);
        if (status != LXB_STATUS_OK) ok = probe_fail("lxb_html_document_parse", status);
    }
    if (ok) {
        printf("html in:  %s\nhtml out: ", html);
        lxb_html_serialize_tree_cb(lxb_dom_interface_node(document), probe_print, NULL);
        printf("\n");
    }

    // CSS: pseudo-element selector and a calc() length both have to parse cleanly.
    if (ok) {
        sheet = lxb_css_stylesheet_create(NULL);
        status = sheet ? lxb_css_stylesheet_parse(sheet, sheet_parser, (const lxb_char_t *) css, sizeof(css) - 1) : LXB_STATUS_ERROR_MEMORY_ALLOCATION;
        if (status != LXB_STATUS_OK) ok = probe_fail("lxb_css_stylesheet_parse", status);
    }
    if (ok) {
        printf("css in:   %s\ncss out:  ", css);
        lxb_css_rule_serialize(sheet->root, probe_print, NULL);
        printf("\n");
    }

    // Selectors against the parsed tree.
    if (ok) {
        lxb_dom_node_t *root = lxb_dom_interface_node(document);
        printf("selectors:\n");
        ok &= probe_expect_matches(selector_parser, selectors, root, "#a.b", 1);
        ok &= probe_expect_matches(selector_parser, selectors, root, "div > input[type]", 1);
        ok &= probe_expect_matches(selector_parser, selectors, root, "div br", 1);
        ok &= probe_expect_matches(selector_parser, selectors, root, "input + input", 0);
    }

    if (selectors) lxb_selectors_destroy(selectors, true);
    if (selector_parser) lxb_css_parser_destroy(selector_parser, true);
    if (selector_memory) lxb_css_memory_destroy(selector_memory, true);
    if (sheet_parser) lxb_css_parser_destroy(sheet_parser, true);
    if (sheet) lxb_css_stylesheet_destroy(sheet, true);
    if (document) lxb_html_document_destroy(document);

    if (!ok) return 1;
    printf("lexbor-probe ok (lexbor %s)\n", LEXBOR_VERSION_STRING);
    return 0;
}

// ------------------------------------------------------------------ engine harness
// Shared by render-page / bench / dump-boxes: a hidden GLFW window (GL 4.5 core,
// glad) whose context renders into an offscreen FBO of exactly w x h, the repo
// root located from the executable (cwd-independent: assets/shaders + ui/), the
// engine initialised through html_ui_init() and a virtual clock so animations
// are deterministic (engine_set_clock_override).

// main.c owns these in the client; ui_test links gl_backend.c without main.c.
unsigned int g_active_texture;
unsigned int g_active_shader;

#define UT_FRAME_MS (1000.0 / 60.0)

typedef struct {
    GLFWwindow *window;
    unsigned fbo, color, depth;
    int w, h;
    char root[1024];   // repo root, trailing slash
    char ui_dir[1100];
} UtHarness;

static int ut_is_root(const char *dir) {
    char path[1200];
    struct stat st;
    snprintf(path, sizeof(path), "%sui/index.html", dir);
    if (stat(path, &st) != 0) return 0;
    snprintf(path, sizeof(path), "%sassets/shaders/html_ui.vert", dir);
    return stat(path, &st) == 0;
}

// Walks up from `start` (a directory path) looking for the repo root.
static int ut_find_root_from(const char *start, char *out, size_t cap) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", start);
    for (int depth = 0; depth < 8; depth++) {
        size_t len = strlen(dir);
        if (len + 2 >= sizeof(dir)) return 0;
        if (!len || dir[len - 1] != '/') {
            dir[len++] = '/';
            dir[len] = 0;
        }
        if (ut_is_root(dir)) {
            snprintf(out, cap, "%s", dir);
            return 1;
        }
        // strip the last component
        dir[len - 1] = 0;
        char *slash = strrchr(dir, '/');
        if (!slash) return 0;
        slash[slash == dir ? 1 : 0] = 0;
        if (slash == dir) {
            if (ut_is_root("/")) { snprintf(out, cap, "/"); return 1; }
            return 0;
        }
    }
    return 0;
}

static int ut_find_root(char *out, size_t cap) {
    const char *env = getenv("KRUNKNATIVE_ROOT");
    if (env && *env) {
        char dir[1024];
        snprintf(dir, sizeof(dir), "%s%s", env, env[strlen(env) - 1] == '/' ? "" : "/");
        if (ut_is_root(dir)) {
            snprintf(out, cap, "%s", dir);
            return 1;
        }
    }
#ifndef _WIN32
    char exe[1024];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = 0;
        char *slash = strrchr(exe, '/');
        if (slash) *slash = 0;
        if (ut_find_root_from(exe, out, cap)) return 1;
    }
#endif
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) && ut_find_root_from(cwd, out, cap)) return 1;
    // out-of-tree build dirs: the source file's own location (absolute under CMake)
    char src[1024];
    snprintf(src, sizeof(src), "%s", __FILE__);
    char *slash = strrchr(src, '/');
    if (slash && src[0] == '/') {
        *slash = 0;
        if (ut_find_root_from(src, out, cap)) return 1;
    }
    return 0;
}

static int ut_harness_init(UtHarness *h, int w, int hgt) {
    memset(h, 0, sizeof(*h));
    h->w = w;
    h->h = hgt;
    if (!ut_find_root(h->root, sizeof(h->root))) {
        fprintf(stderr, "ui_test: cannot locate the repo root (ui/index.html + assets/shaders); set KRUNKNATIVE_ROOT\n");
        return 0;
    }
    // assets_path() is cwd-relative ("./assets/" from the root)
    if (chdir(h->root) != 0) {
        fprintf(stderr, "ui_test: chdir %s failed\n", h->root);
        return 0;
    }
    snprintf(h->ui_dir, sizeof(h->ui_dir), "%sui/", h->root);

    if (!headless_glfw_init()) {
        fprintf(stderr, "ui_test: glfwInit failed (no display? run under xvfb-run or with --headless)\n");
        return 0;
    }
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
#ifdef __APPLE__
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);  // macOS core profile cap; shader.c rewrites to 410 core
#else
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
#endif
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    headless_window_hints();
    h->window = glfwCreateWindow(64, 64, "ui_test", NULL, NULL);
    if (!h->window) {
        fprintf(stderr, "ui_test: glfwCreateWindow (GL 4.5 core) failed\n");
        glfwTerminate();
        return 0;
    }
    if (!headless_make_context_current(h->window)) {
        fprintf(stderr, "ui_test: no GL 4.5 core context\n");
        glfwTerminate();
        return 0;
    }
    if (!gladLoadGLLoader(headless_get_proc_address)) {
        fprintf(stderr, "ui_test: glad failed\n");
        return 0;
    }

    glGenTextures(1, &h->color);
    glBindTexture(GL_TEXTURE_2D, h->color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, hgt, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenRenderbuffers(1, &h->depth);
    glBindRenderbuffer(GL_RENDERBUFFER, h->depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, hgt);
    glGenFramebuffers(1, &h->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, h->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, h->color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, h->depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "ui_test: offscreen framebuffer incomplete\n");
        return 0;
    }
    glViewport(0, 0, w, hgt);
    // the client's global blend state (main.c)
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    engine_set_clock_override(0.0);
    if (!html_ui_init(NULL, h->ui_dir, w, hgt)) {
        fprintf(stderr, "ui_test: html_ui_init(%s) failed\n", h->ui_dir);
        return 0;
    }
    return 1;
}

static void ut_harness_fini(UtHarness *h) {
    html_ui_fini();
    if (h->fbo) glDeleteFramebuffers(1, &h->fbo);
    if (h->depth) glDeleteRenderbuffers(1, &h->depth);
    if (h->color) glDeleteTextures(1, &h->color);
    headless_shutdown();
    if (h->window) glfwDestroyWindow(h->window);
    glfwTerminate();
}

// One client frame: update (timers, animations, observers) + composite.
static void ut_frame(UtHarness *h, double clock_ms, void (*background)(void *), void *bg_user) {
    engine_set_clock_override(clock_ms);
    html_ui_update();
    glBindFramebuffer(GL_FRAMEBUFFER, h->fbo);
    glViewport(0, 0, h->w, h->h);
    if (background) background(bg_user);
    UI ui;
    memset(&ui, 0, sizeof(ui));
    ui.width = (float) h->w;
    ui.height = (float) h->h;
    ui.scale = 1.0f;
    html_ui_render(&ui);
}

// ---- DOM presets -----------------------------------------------------------

static HtmlElem ut_elem(const char *id) {
    HtmlElem e = html_dom_by_id(id);
    if (!e && !strcmp(id, "html")) e = html_dom_root();
    if (!e && !strcmp(id, "body")) e = html_dom_body();
    return e;
}

static void ut_style(const char *id, const char *prop, const char *value) {
    const HtmlElem e = ut_elem(id);
    if (e) html_dom_set_style(e, prop, value);
    else fprintf(stderr, "ui_test: #%s not found (style %s)\n", id, prop);
}

// krunknative.js updatePreviewHole(canvas): cut #instructionHolder's 50 % dim
// open over #classPreviewCanvas (the engine draws the class preview underneath).
static void ut_preview_hole(void) {
    const HtmlElem holder = ut_elem("instructionHolder"), canvas = ut_elem("classPreviewCanvas");
    HtmlRect hr, cr;
    if (!holder || !canvas || !html_dom_get_rect(holder, &hr) || !html_dom_get_rect(canvas, &cr) || !cr.w || !cr.h) {
        if (holder) html_dom_set_class(holder, "kn-hole", 0);
        return;
    }
    HtmlElem hole = ut_elem("knPreviewHole");
    if (!hole) {
        hole = html_dom_create("div", "knPreviewHole");
        html_dom_append(holder, hole);
    }
    const float ow = html_dom_offset_width(holder);
    float k = ow > 0.0f ? hr.w / ow : 1.0f;
    if (k == 0.0f) k = 1.0f;
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2fpx", (cr.x - hr.x) / k);
    html_dom_set_style(hole, "left", buf);
    snprintf(buf, sizeof(buf), "%.2fpx", (cr.y - hr.y) / k);
    html_dom_set_style(hole, "top", buf);
    snprintf(buf, sizeof(buf), "%.2fpx", cr.w / k);
    html_dom_set_style(hole, "width", buf);
    snprintf(buf, sizeof(buf), "%.2fpx", cr.h / k);
    html_dom_set_style(hole, "height", buf);
    html_dom_set_class(holder, "kn-hole", 1);
}

// krunknative.js setupDom() + applyMenuOpen(true) + updatePlayInstructions()
// (connected, map ready: "CLICK TO PLAY") + updateClassBox() for class 0 / Guest
// + scaleBase() with the default scaleUI 0.8 for a w x h viewport +
// sendPreviewRect()'s updatePreviewHole().
static void ut_menu_preset(int w, int h) {
    HtmlElem e;
    // setupDom()
    ut_style("loadingBg", "display", "none");
    ut_style("consentBlock", "display", "none");
    ut_style("bdayBlock", "display", "none");
    ut_style("menuFPSDisplay", "display", "none");
    ut_style("vignette", "opacity", "0");
    ut_style("teamSelector", "display", "none");
    ut_style("classPreviewCanvas", "width", "660px");
    ut_style("classPreviewCanvas", "height", "350px");
    ut_style("bubbleContainer", "transform", "scale(0.0)");
    html_dom_set_class(html_dom_root(), "kn-menu-open", 1);
    ut_style("menuHolder", "display", "block");
    ut_style("speakerDisplay", "display", "none");
    ut_style("chatHolder", "bottom", "20px");
    ut_style("voiceDisplay", "bottom", "18px");
    ut_style("supportMapCreator", "display", "none");
    ut_style("menuHider", "display", "block");
    if ((e = ut_elem("uiBase"))) html_dom_set_class(e, "onMenu", 1);
    ut_style("gameNameHolder", "display", "");
    ut_style("spectButton", "display", "");
    ut_style("spinUI", "display", "none");
    ut_style("respinUI", "display", "none");
    ut_style("endUI", "display", "none");
    ut_style("instructionHolder", "display", "block");
    {
        char disp[32] = "";
        const HtmlElem wh = ut_elem("windowHolder");
        if (wh) html_dom_get_style(wh, "display", disp, sizeof(disp));
        if (strcmp(disp, "block") != 0) ut_style("baseLinks", "display", "block");
    }
    ut_style("newLeaderDisplay", "display", "none");
    ut_style("terminalHolder", "display", "none");
    ut_style("menuClassContainer", "visibility", "");
    // updatePlayInstructions(): canPlay()
    if ((e = ut_elem("instructions"))) html_dom_set_html(e, "CLICK TO PLAY");
    ut_style("instructionHolder", "pointer-events", "all");
    // updateClassBox(): class 0
    if ((e = ut_elem("menuClassName"))) html_dom_set_text(e, "Triggerman");
    if ((e = ut_elem("menuClassSubtext"))) html_dom_set_html(e, "<span style='color:inherit'>Assault Rifle</span>");
    if ((e = ut_elem("menuClassNameTag"))) html_dom_set_html(e, "<span class='menuClassPlayerName'>Guest</span>");
    // scaleBase(): custom = 1 - 0.8 + 1 = 1.2 -> 2040 x 1080 design space
    const double custom = 1.0 - 0.8 + 1.0;
    const double dw = custom * 1700.0, dh = custom * 900.0;
    const double sx = w / dw, sy = h / dh;
    char buf[64];
    if (sy < sx) {
        snprintf(buf, sizeof(buf), "scale(%.3f)", sy);
        ut_style("uiBase", "transform", buf);
        snprintf(buf, sizeof(buf), "%.3fpx", w / sy);
        ut_style("uiBase", "width", buf);
        snprintf(buf, sizeof(buf), "%gpx", dh);
        ut_style("uiBase", "height", buf);
    } else {
        snprintf(buf, sizeof(buf), "scale(%.3f)", sx);
        ut_style("uiBase", "transform", buf);
        snprintf(buf, sizeof(buf), "%gpx", dw);
        ut_style("uiBase", "width", buf);
        snprintf(buf, sizeof(buf), "%.3fpx", h / sx);
        ut_style("uiBase", "height", buf);
    }
    ut_preview_hole();
    html_ui_set_menu_open(1);
}

// "--set id:prop:value" / "--class id:cls"
static int ut_apply_set(const char *spec) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", spec);
    char *a = strchr(buf, ':');
    char *b = a ? strchr(a + 1, ':') : NULL;
    if (!a || !b) {
        fprintf(stderr, "ui_test: --set expects id:prop:value, got '%s'\n", spec);
        return 0;
    }
    *a = 0;
    *b = 0;
    ut_style(buf, a + 1, b + 1);
    return 1;
}

static int ut_apply_class(const char *spec) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", spec);
    char *a = strchr(buf, ':');
    if (!a) {
        fprintf(stderr, "ui_test: --class expects id:cls, got '%s'\n", spec);
        return 0;
    }
    *a = 0;
    const int off = a[1] == '-';  // "id:-cls" removes
    const HtmlElem e = ut_elem(buf);
    if (!e) {
        fprintf(stderr, "ui_test: #%s not found (class)\n", buf);
        return 0;
    }
    html_dom_set_class(e, a + 1 + off, !off);
    return 1;
}

static int ut_hover(const char *selector) {
    const HtmlElem e = html_dom_query(0, selector);
    HtmlRect r;
    if (!e || !html_dom_get_rect(e, &r)) {
        fprintf(stderr, "ui_test: --hover '%s': no laid-out match\n", selector);
        return 0;
    }
    html_ui_mouse_move(r.x + r.w * 0.5, r.y + r.h * 0.5);
    return 1;
}

// ---- background ------------------------------------------------------------

typedef struct {
    float rgb[3];
    unsigned tex, read_fbo;
    int tw, th, w, h;
} UtBackground;

static int ut_background_init(UtBackground *bg, const char *spec, int w, int h) {
    memset(bg, 0, sizeof(*bg));
    bg->w = w;
    bg->h = h;
    if (spec[0] == '#' && strlen(spec) == 7) {
        const unsigned v = (unsigned) strtoul(spec + 1, NULL, 16);
        bg->rgb[0] = ((v >> 16) & 255) / 255.0f;
        bg->rgb[1] = ((v >> 8) & 255) / 255.0f;
        bg->rgb[2] = (v & 255) / 255.0f;
        return 1;
    }
    int n = 0;
    stbi_set_flip_vertically_on_load_thread(0);
    unsigned char *px = stbi_load(spec, &bg->tw, &bg->th, &n, 4);
    if (!px) {
        fprintf(stderr, "ui_test: background '%s' is neither #rrggbb nor a readable image\n", spec);
        return 0;
    }
    glGenTextures(1, &bg->tex);
    glBindTexture(GL_TEXTURE_2D, bg->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, bg->tw, bg->th, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(px);
    glGenFramebuffers(1, &bg->read_fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, bg->read_fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, bg->tex, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    return 1;
}

static void ut_background_draw(void *user) {
    UtBackground *bg = user;
    glClearColor(bg->rgb[0], bg->rgb[1], bg->rgb[2], 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    if (!bg->tex) return;
    // image rows are top-down: flip while stretching over the framebuffer
    GLint draw = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, bg->read_fbo);
    glBlitFramebuffer(0, 0, bg->tw, bg->th, 0, bg->h, bg->w, 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint) draw);
}

static void ut_background_fini(UtBackground *bg) {
    if (bg->read_fbo) glDeleteFramebuffers(1, &bg->read_fbo);
    if (bg->tex) glDeleteTextures(1, &bg->tex);
}

// ---- PNG writer: client/src/png_write.c (png_write_rgba) ---------------------

static int ut_read_back_png(UtHarness *h, const char *path) {
    const size_t stride = (size_t) h->w * 4;
    unsigned char *px = malloc(stride * (size_t) h->h);
    unsigned char *flip = malloc(stride * (size_t) h->h);
    if (!px || !flip) {
        free(px);
        free(flip);
        return 0;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, h->fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, h->w, h->h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (int y = 0; y < h->h; y++) {
        memcpy(flip + (size_t) y * stride, px + (size_t) (h->h - 1 - y) * stride, stride);
        for (int x = 0; x < h->w; x++) flip[(size_t) y * stride + (size_t) x * 4 + 3] = 255;
    }
    const int ok = png_write_rgba(path, flip, h->w, h->h);
    free(px);
    free(flip);
    return ok;
}

// ---- common option parsing -----------------------------------------------------

typedef struct {
    int menu_open, menu_preset, frames;
    double anim_time_s;     // < 0: frames advance from 0 at 60 Hz
    const char *sets[64];
    int n_sets;
    const char *classes[64];
    int n_classes;
    const char *hover;
} UtPageOpts;

// Parses the shared flags from argv[first..]; returns 0 on a usage error.
static int ut_parse_page_opts(int argc, char **argv, int first, UtPageOpts *o) {
    memset(o, 0, sizeof(*o));
    o->frames = 3;
    o->anim_time_s = -1.0;
    for (int i = first; i < argc; i++) {
        const char *a = argv[i];
        const int has_next = i + 1 < argc;
        if (!strcmp(a, "--menu-open")) o->menu_open = 1;
        else if (!strcmp(a, "--menu-preset")) o->menu_preset = 1;
        else if (!strcmp(a, "--frames") && has_next) o->frames = atoi(argv[++i]);
        else if (!strcmp(a, "--anim-time") && has_next) o->anim_time_s = atof(argv[++i]);
        else if (!strcmp(a, "--set") && has_next && o->n_sets < 64) o->sets[o->n_sets++] = argv[++i];
        else if (!strcmp(a, "--class") && has_next && o->n_classes < 64) o->classes[o->n_classes++] = argv[++i];
        else if (!strcmp(a, "--hover") && has_next) o->hover = argv[++i];
        else {
            fprintf(stderr, "ui_test: unknown or incomplete option '%s'\n", a);
            return 0;
        }
    }
    if (o->frames < 1) o->frames = 1;
    return 1;
}

static void ut_apply_page_opts(const UtPageOpts *o, int w, int h) {
    if (o->menu_preset) ut_menu_preset(w, h);
    else if (o->menu_open) {
        html_dom_set_class(html_dom_root(), "kn-menu-open", 1);
        html_ui_set_menu_open(1);
    }
    for (int i = 0; i < o->n_classes; i++) ut_apply_class(o->classes[i]);
    for (int i = 0; i < o->n_sets; i++) ut_apply_set(o->sets[i]);
}

static double ut_frame_clock(const UtPageOpts *o, int i) {
    if (o->anim_time_s >= 0.0) return o->anim_time_s * 1000.0 - (o->frames - 1 - i) * UT_FRAME_MS;
    return (i + 1) * UT_FRAME_MS;
}

// ------------------------------------------------------------------ render-page

static int cmd_render_page(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: ui_test render-page <w> <h> <#rrggbb|image> <out.png> [--menu-open] [--menu-preset]\n"
                        "         [--frames N] [--anim-time S] [--set id:prop:value]... [--class id:cls]... [--hover selector]\n"
                        "  --menu-preset  applies krunknative.js applyMenuOpen(true) + CLICK TO PLAY + class box + scaleBase()\n"
                        "  --class id:-cls removes a class; id 'html'/'body' address the root/body elements\n");
        return 2;
    }
    const int w = atoi(argv[1]), h = atoi(argv[2]);
    const char *bg_spec = argv[3], *out = argv[4];
    UtPageOpts o;
    if (w <= 0 || h <= 0 || !ut_parse_page_opts(argc, argv, 5, &o)) return 2;

    UtHarness hs;
    if (!ut_harness_init(&hs, w, h)) {
        ut_harness_fini(&hs);
        return 1;
    }
    UtBackground bg;
    if (!ut_background_init(&bg, bg_spec, w, h)) {
        ut_harness_fini(&hs);
        return 1;
    }
    ut_apply_page_opts(&o, w, h);
    for (int i = 0; i < o.frames; i++) {
        if (i == 1 && o.hover) ut_hover(o.hover);  // after the first layout
        ut_frame(&hs, ut_frame_clock(&o, i), ut_background_draw, &bg);
    }
    if (o.hover && o.frames == 1) {
        ut_hover(o.hover);
        ut_frame(&hs, ut_frame_clock(&o, 0) + 1.0, ut_background_draw, &bg);
    }
    glFinish();
    const int ok = ut_read_back_png(&hs, out);
    if (ok) printf("render-page: %dx%d -> %s\n", w, h, out);
    ut_background_fini(&bg);
    ut_harness_fini(&hs);
    return ok ? 0 : 1;
}

// ------------------------------------------------------------------ bench

static int cmd_bench(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: ui_test bench <w> <h> [--frames N] [--no-preset] [page options of render-page]\n");
        return 2;
    }
    const int w = atoi(argv[1]), h = atoi(argv[2]);
    int no_preset = 0;
    // strip --no-preset before the shared parser
    char *rest[128];
    int n_rest = 0;
    for (int i = 3; i < argc && n_rest < 128; i++) {
        if (!strcmp(argv[i], "--no-preset")) no_preset = 1;
        else rest[n_rest++] = argv[i];
    }
    char *args[130];
    args[0] = argv[0];
    for (int i = 0; i < n_rest; i++) args[i + 1] = rest[i];
    UtPageOpts o;
    if (w <= 0 || h <= 0 || !ut_parse_page_opts(n_rest + 1, args, 1, &o)) return 2;
    if (!no_preset) o.menu_preset = 1;
    int frames_given = 0;
    for (int i = 0; i < n_rest; i++) frames_given |= !strcmp(rest[i], "--frames");
    if (!frames_given) o.frames = 300;

    UtHarness hs;
    if (!ut_harness_init(&hs, w, h)) {
        ut_harness_fini(&hs);
        return 1;
    }
    UtBackground bg;
    ut_background_init(&bg, "#6b6363", w, h);
    ut_apply_page_opts(&o, w, h);
    // warm-up frame (first full restyle/layout/paint + atlas fill), measured apart
    double t0 = glfwGetTime();
    ut_frame(&hs, UT_FRAME_MS, ut_background_draw, &bg);
    glFinish();
    const double first_ms = (glfwGetTime() - t0) * 1000.0;
    EngineStats first;
    engine_stats(&first, 1);

    double max_ms = 0.0, sum_ms = 0.0;
    for (int i = 1; i <= o.frames; i++) {
        const double f0 = glfwGetTime();
        ut_frame(&hs, (i + 1) * UT_FRAME_MS, ut_background_draw, &bg);
        glFinish();
        const double ms = (glfwGetTime() - f0) * 1000.0;
        sum_ms += ms;
        if (ms > max_ms) max_ms = ms;
    }
    EngineStats s;
    engine_stats(&s, 0);
    const double n = (double) o.frames;
    printf("bench %dx%d, %d frames (virtual 60 Hz clock, glFinish per frame)\n", w, h, o.frames);
    printf("  first frame   %.3f ms (restyle %.3f, layout %.3f, paint %.3f, %u elements styled)\n", first_ms,
           first.restyle_ms, first.layout_ms, first.paint_ms, first.restyled_elems);
    printf("  frame total   avg %.3f ms  max %.3f ms\n", sum_ms / n, max_ms);
    printf("  restyle       avg %.3f ms/frame  (%u passes, %.3f ms each, %u elements)\n", s.restyle_ms / n, s.restyles,
           s.restyles ? s.restyle_ms / s.restyles : 0.0, s.restyled_elems);
    printf("  layout        avg %.3f ms/frame  (%u passes incl. %u box-tree rebuilds, %.3f ms each)\n", s.layout_ms / n,
           s.layouts, s.rebuilds, s.layouts ? s.layout_ms / s.layouts : 0.0);
    printf("  paint         avg %.3f ms/frame  (%u builds, %.3f ms each)\n", s.paint_ms / n, s.paints,
           s.paints ? s.paint_ms / s.paints : 0.0);
    printf("  submit        avg %.3f ms/frame  (CPU side; %d draw calls, %d vertices, %d display-list items)\n",
           s.submit_ms / n, s.draw_calls, s.vertices, s.dl_items);
    ut_background_fini(&bg);
    ut_harness_fini(&hs);
    return 0;
}

// ------------------------------------------------------------------ bench-dynamic
// The menu under load: every frame the pointer moves to the next .menuItem (a
// :hover restyle per frame), and at 20 Hz (every 3rd frame) the HUD is updated
// like hud_ui.c does (#ammoVal text + #ammoDisplay background-size).  --hud
// also shows #inGameUI so the HUD writes hit rendered boxes.

static int cmd_bench_dynamic(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: ui_test bench-dynamic <w> <h> [--frames N] [--hud]\n");
        return 2;
    }
    const int w = atoi(argv[1]), h = atoi(argv[2]);
    int frames = 300, hud = 0;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--hud")) hud = 1;
        else {
            fprintf(stderr, "ui_test: unknown option '%s'\n", argv[i]);
            return 2;
        }
    }
    if (w <= 0 || h <= 0 || frames < 1) return 2;
    UtHarness hs;
    if (!ut_harness_init(&hs, w, h)) {
        ut_harness_fini(&hs);
        return 1;
    }
    UtBackground bg;
    ut_background_init(&bg, "#6b6363", w, h);
    ut_menu_preset(w, h);
    if (hud) ut_style("inGameUI", "display", "block");
    ut_frame(&hs, UT_FRAME_MS, ut_background_draw, &bg);
    glFinish();

    HtmlElem items[64];
    const int n_items = html_dom_query_all(0, ".menuItem", items, 64);
    float cx[64], cy[64];
    int n_targets = 0;
    for (int i = 0; i < n_items && i < 64; i++) {
        HtmlRect r;
        if (!html_dom_get_rect(items[i], &r) || r.w <= 0 || r.h <= 0) continue;
        cx[n_targets] = r.x + r.w * 0.5f;
        cy[n_targets] = r.y + r.h * 0.5f;
        n_targets++;
    }
    const HtmlElem ammo = html_dom_by_id("ammoVal"), ammo_bar = html_dom_by_id("ammoDisplay");
    engine_stats(NULL, 1);
    double sum = 0.0, max = 0.0;
    int hud_writes = 0;
    for (int i = 0; i < frames; i++) {
        const double f0 = glfwGetTime();
        if (n_targets) html_ui_mouse_move(cx[i % n_targets], cy[i % n_targets]);
        if (getenv("UT_DEBUG_HOVER") && i < n_targets) {
            const HtmlElem hv = events_hovered(engine_events(engine_get()));
            char id[64] = "", cls[64] = "", tag[16] = "";
            html_dom_get_attr(hv, "id", id, sizeof(id));
            html_dom_get_attr(hv, "class", cls, sizeof(cls));
            html_dom_tag(hv, tag, sizeof(tag));
            fprintf(stderr, "hover (%.0f,%.0f) -> %u <%s#%s.%s>\n", cx[i], cy[i], hv, tag, id, cls);
        }
        if (i % 3 == 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%d", 30 - (i / 3) % 31);
            if (ammo) html_dom_set_text(ammo, buf);
            snprintf(buf, sizeof(buf), "%d%% auto", 100 - (i / 3) % 101);
            if (ammo_bar) html_dom_set_style(ammo_bar, "background-size", buf);
            hud_writes++;
        }
        ut_frame(&hs, (i + 2) * UT_FRAME_MS, ut_background_draw, &bg);
        glFinish();
        const double ms = (glfwGetTime() - f0) * 1000.0;
        sum += ms;
        if (ms > max) max = ms;
    }
    EngineStats s;
    engine_stats(&s, 0);
    const double n = (double) frames;
    printf("bench-dynamic %dx%d, %d frames: hover sweep over %d .menuItem boxes, %d HUD writes%s\n", w, h, frames, n_targets,
           hud_writes, hud ? " (#inGameUI shown)" : "");
    printf("  frame total   avg %.3f ms  max %.3f ms\n", sum / n, max);
    printf("  restyle       avg %.3f ms/frame  (%u passes, %u elements)\n", s.restyle_ms / n, s.restyles, s.restyled_elems);
    printf("  layout        avg %.3f ms/frame  (%u passes, %u full box-tree rebuilds, %u incremental rebuilds)\n", s.layout_ms / n,
           s.layouts, s.rebuilds, s.partial_rebuilds);
    printf("  paint         avg %.3f ms/frame  (%u builds)\n", s.paint_ms / n, s.paints);
    printf("  submit        avg %.3f ms/frame\n", s.submit_ms / n);
    ut_background_fini(&bg);
    ut_harness_fini(&hs);
    return 0;
}

// ------------------------------------------------------------------ dump-boxes

static void ut_print_line(const char *line, void *user) {
    (void) user;
    puts(line);
}

static int cmd_dump_boxes(int argc, char **argv) {
    const char *selector = NULL;
    int w = 1280, h = 720;
    char *rest[128];
    int n_rest = 0;
    for (int i = 1; i < argc && n_rest < 128; i++) {
        if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &w, &h) != 2) return 2;
        } else if (argv[i][0] != '-' && !selector) {
            selector = argv[i];
        } else {
            rest[n_rest++] = argv[i];
        }
    }
    char *args[130];
    args[0] = argv[0];
    for (int i = 0; i < n_rest; i++) args[i + 1] = rest[i];
    UtPageOpts o;
    if (!ut_parse_page_opts(n_rest + 1, args, 1, &o)) return 2;

    UtHarness hs;
    if (!ut_harness_init(&hs, w, h)) {
        ut_harness_fini(&hs);
        return 1;
    }
    ut_apply_page_opts(&o, w, h);
    for (int i = 0; i < o.frames; i++) ut_frame(&hs, ut_frame_clock(&o, i), NULL, NULL);
    HtmlElem e = 0;
    if (selector) {
        e = html_dom_query(0, selector);
        if (!e) {
            fprintf(stderr, "dump-boxes: no element matches '%s'\n", selector);
            ut_harness_fini(&hs);
            return 1;
        }
    }
    engine_dump_boxes(engine_get(), e, ut_print_line, NULL);
    ut_harness_fini(&hs);
    return 0;
}

// ------------------------------------------------------------------ subcommand table
//
// HOW TO ADD A SUBCOMMAND: implement `int ui_test_cmd_<name>(int argc, char **argv)`
// in your engine module (declare it in that module's header, or add an extern
// prototype right here) and append one row below. Keep names kebab-case and put
// `help` first; `ui_test help` prints this table. Everything compiled into ui_test is
// listed in the ui_test target in the root CMakeLists.txt.

static int cmd_help(int argc, char **argv);

static const UiTestSubcommand g_subcommands[] = {
    {"help",         "",  "list subcommands",                                                        cmd_help},
    {"lexbor-probe", "",  "parse HTML + CSS and match selectors with the vendored Lexbor (smoke test)", cmd_lexbor_probe},
    {"render-page",  "<w> <h> <bg> <out.png> [opts]", "render ui/index.html offscreen to PNG (--menu-preset, --set, --class, --hover, --frames, --anim-time)", cmd_render_page},
    {"bench",        "<w> <h> [--frames N]", "per-stage frame timings of the menu (restyle/layout/paint/submit)",              cmd_bench},
    {"bench-dynamic", "<w> <h> [--frames N] [--hud]", "menu hover sweep + 20 Hz HUD writes: per-stage timings", cmd_bench_dynamic},
    {"dump-boxes",   "[selector] [--size WxH]", "print the box tree with local + viewport rects (accepts render-page page options)", cmd_dump_boxes},
};

// ------------------------------------------------------------------ driver

static void usage(FILE *out) {
    fprintf(out, "usage: ui_test [--headless] <subcommand> [args...]\n\nsubcommands:\n");
    for (size_t i = 0; i < sizeof(g_subcommands) / sizeof(g_subcommands[0]); i++) {
        const UiTestSubcommand *cmd = &g_subcommands[i];
        char head[64];
        snprintf(head, sizeof(head), "%s %s", cmd->name, cmd->args);
        fprintf(out, "  %-28s %s\n", head, cmd->help);
    }
}

static int cmd_help(const int argc, char **argv) {
    (void) argc;
    (void) argv;
    usage(stdout);
    return 0;
}

int main(int argc, char **argv) {
    headless_parse_args(&argc, argv);  // `--headless` anywhere: offscreen EGL context, no window
    if (argc < 2) {
        usage(stderr);
        return 2;
    }

    for (size_t i = 0; i < sizeof(g_subcommands) / sizeof(g_subcommands[0]); i++) {
        if (!strcmp(argv[1], g_subcommands[i].name)) return g_subcommands[i].fn(argc - 1, argv + 1);
    }

    fprintf(stderr, "ui_test: unknown subcommand '%s'\n\n", argv[1]);
    usage(stderr);
    return 2;
}
