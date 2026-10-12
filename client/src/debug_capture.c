// Camera-matched world captures (see client/include/debug_capture.h and
// doc/ref/krunker-v3.1.1/world/README.md). Driven only by environment variables so the
// normal client is untouched when they are absent.
#include <gl.h>
#include <GLFW/glfw3.h>
#include <debug_capture.h>
#include <png_write.h>
#include <html_ui.h>
#include <html_dom.h>
#include <console.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h> // mkdtemp (macOS declares it here, not in stdlib.h)
#endif

typedef struct {
    int initialized;
    int pinned;          // KRUNKNATIVE_CAMERA parsed
    vec3 position;
    float pitch;
    float yaw;
    float fov;           // radians, 0 = keep the client's FOV
    float ortho;         // KRUNKNATIVE_ORTHO: orthographic half height (world units), 0 = perspective
    float far;           // KRUNKNATIVE_CAMERA_FAR: camera far plane, 0 = keep
    int no_fog;          // KRUNKNATIVE_NO_FOG=1: fog off (editor captures, tools/editor-mcp)
    int hide_ui;
    const char *screenshot;
    int frames_wanted;
    int frames_drawn;
    int done;
} DebugCapture;

static DebugCapture g_capture;

void debug_capture_startup(void) {
    const char *camera = getenv("KRUNKNATIVE_CAMERA");
    const char *screenshot = getenv("KRUNKNATIVE_SCREENSHOT");
    if (!(camera && *camera) && !(screenshot && *screenshot)) return;

    // A capture must reproduce its pose with v3.1.1 defaults: the user's stored settings
    // (fov, aspectRatio, resolution, lighting, ...) live under $XDG_CONFIG_HOME/krunknative,
    // so unless the caller chose a config dir (setting runs do) use an empty private one.
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return;

#ifdef _WIN32
    _putenv_s("XDG_CONFIG_HOME", "krunknative-capture-config");
#else
    char dir[] = "/tmp/krunknative-capture-XXXXXX";
    if (mkdtemp(dir)) setenv("XDG_CONFIG_HOME", dir, 1);
#endif
}

static void debug_capture_init(void) {
    if (g_capture.initialized) return;
    g_capture.initialized = 1;

    const char *camera = getenv("KRUNKNATIVE_CAMERA");
    if (camera && *camera) {
        float x, y, z, pitch, yaw;

        if (sscanf(camera, "%f,%f,%f,%f,%f", &x, &y, &z, &pitch, &yaw) == 5) {
            g_capture.pinned = 1;
            g_capture.position = (vec3) {x, y, z};
            g_capture.pitch = pitch;
            g_capture.yaw = yaw;
        } else {
            fprintf(stderr, "KRUNKNATIVE_CAMERA: expected \"x,y,z,pitch,yaw\", got \"%s\"\n", camera);
        }
    }

    const char *fov = getenv("KRUNKNATIVE_FOV");
    if (fov && *fov) g_capture.fov = (float) atof(fov) * (float) M_PI / 180.0f;

    // editor captures (tools/editor-mcp render_*): orthographic views, no fog, far plane
    const char *ortho = getenv("KRUNKNATIVE_ORTHO");
    if (ortho && *ortho) g_capture.ortho = (float) atof(ortho);
    const char *far = getenv("KRUNKNATIVE_CAMERA_FAR");
    if (far && *far) g_capture.far = (float) atof(far);
    const char *no_fog = getenv("KRUNKNATIVE_NO_FOG");
    g_capture.no_fog = no_fog && !strcmp(no_fog, "1");

    const char *hide_ui = getenv("KRUNKNATIVE_HIDE_UI");
    g_capture.hide_ui = hide_ui && !strcmp(hide_ui, "1");

    const char *screenshot = getenv("KRUNKNATIVE_SCREENSHOT");
    if (screenshot && *screenshot) g_capture.screenshot = screenshot;

    const char *frames = getenv("KRUNKNATIVE_CAPTURE_FRAMES");
    g_capture.frames_wanted = frames && *frames ? atoi(frames) : 30;
    if (g_capture.frames_wanted < 1) g_capture.frames_wanted = 1;
}

void debug_capture_apply_camera(Client *client) {
    debug_capture_init();

    if (g_capture.fov > 0.0f) client->camera.fov = g_capture.fov;
    if (g_capture.far > 0.0f) client->camera.far = g_capture.far;
    if (g_capture.no_fog) g_render_lighting.fog_enabled = 0;
    if (!g_capture.pinned) return;
    client->camera.ortho = g_capture.ortho > 0.0f ? g_capture.ortho : 0.0f;

    client->camera.position = g_capture.position;
    client->camera.rotation.x = g_capture.pitch;
    client->camera.rotation.y = g_capture.yaw;
    client->camera.rotation.z = 0.0f;
    client->camera.zoom = 1.0f;
}

int debug_capture_hide_ui(void) {
    debug_capture_init();
    return g_capture.hide_ui;
}

static void debug_capture_write(Client *client) {
    int width, height;
    glfwGetFramebufferSize(client->window, &width, &height);
    if (width <= 0 || height <= 0) return;

    unsigned char *pixels = malloc((size_t) width * (size_t) height * 4);
    unsigned char *flipped = malloc((size_t) width * (size_t) height * 4);

    if (pixels && flipped) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

        const size_t row = (size_t) width * 4;
        for (int y = 0; y < height; y++) {
            memcpy(flipped + (size_t) y * row, pixels + (size_t) (height - 1 - y) * row, row);
        }

        // the default framebuffer may carry translucent alpha from blended draws
        for (size_t i = 3; i < row * (size_t) height; i += 4) flipped[i] = 255;

        if (png_write_rgba(g_capture.screenshot, flipped, width, height)) {
            fprintf(stderr, "debug capture: wrote %s (%dx%d)\n", g_capture.screenshot, width, height);
        }
    }

    free(pixels);
    free(flipped);
}

static void ui_cmds_poll(Client *client);

void debug_capture_frame_end(Client *client) {
    debug_capture_init();
    ui_cmds_poll(client);
    if (!g_capture.screenshot || g_capture.done) return;
    if (!client->game.ready || !client->map_loaded) return;

    if (++g_capture.frames_drawn < g_capture.frames_wanted) return;

    debug_capture_write(client);
    g_capture.done = 1;
    glfwSetWindowShouldClose(client->window, GLFW_TRUE);
}

/* ------------------------------------------------------------ KRUNKNATIVE_UI_CMDS */
// A command file the client tails every frame end (after the page is composited), so a
// test can drive a running client like CDP drives Chromium (tools/kref/interactions/
// kn-drive.sh). Append one command per line:
//   ui showWindow(14);selectClass(2)   inline-handler calls dispatched on the shown page
//   move x y | click x y | scroll dy    pointer through the window's GLFW callbacks (fb px)
//   clickid <id>                        click the centre of #id
//   key <glfw key> [press|release]      key through the GLFW key callback (default: both)
//   text <chars>                        characters through the char callback
//   con <line>                          developer console line (ui_console_exec)
//   shot <out.png>                      write this frame (world + page) to a PNG
//   rect <id>                           print #id's rect to stderr
//   at x y                              print the elementFromPoint chain at (x, y) to stderr
//   quit                                close the window
static struct {
    long offset;
    int cooldown;
    char pending[1024];
} g_ui_cmds;

static int ui_cmds_args(const char *s, HtmlArg *args, char strs[][128], const int max) {
    int argc = 0;
    while (*s && *s != ')' && argc < max) {
        while (isspace((unsigned char) *s) || *s == ',') s++;
        if (*s == ')' || !*s) break;
        HtmlArg *a = &args[argc];
        memset(a, 0, sizeof(*a));
        if (*s == '\'' || *s == '"') {
            const char q = *s++;
            size_t n = 0;
            while (*s && *s != q && n + 1 < 128) strs[argc][n++] = *s++;
            strs[argc][n] = 0;
            if (*s == q) s++;
            a->kind = HTML_ARG_STR;
            a->str = strs[argc];
        } else if (!strncmp(s, "true", 4) || !strncmp(s, "false", 5)) {
            a->kind = HTML_ARG_BOOL;
            a->num = *s == 't';
            s += *s == 't' ? 4 : 5;
        } else {
            char *end;
            a->kind = HTML_ARG_NUM;
            a->num = strtod(s, &end);
            if (end == s) { // bare identifier: keep its text
                size_t n = 0;
                while (*s && *s != ',' && *s != ')' && n + 1 < 128) strs[argc][n++] = *s++;
                strs[argc][n] = 0;
                a->kind = HTML_ARG_STR;
                a->str = strs[argc];
            } else {
                s = end;
            }
        }
        argc++;
    }
    return argc;
}

static void ui_cmds_dispatch(char *calls) {
    for (char *call = strtok(calls, ";"); call; call = strtok(NULL, ";")) {
        while (isspace((unsigned char) *call)) call++;
        char *paren = strchr(call, '(');
        if (!paren) continue;
        char name[128];
        snprintf(name, sizeof(name), "%.*s", (int) (paren - call), call);
        // window.windows[0].changeTab -> windows.0.changeTab (the engine's normalisation)
        char norm[128];
        size_t n = 0;
        for (const char *p = !strncmp(name, "window.", 7) ? name + 7 : name; *p && n + 1 < sizeof(norm); p++) {
            if (*p == '[') norm[n++] = '.';
            else if (*p != ']') norm[n++] = *p;
        }
        norm[n] = 0;
        HtmlArg args[8];
        char strs[8][128];
        const int argc = ui_cmds_args(paren + 1, args, strs, 8);
        HtmlEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = "click";
        ev.value = "";
        const int handled = html_ui_dispatch(norm, &ev, args, argc);
        fprintf(stderr, "[ui_cmds] %s/%d -> %d\n", norm, argc, handled);
    }
}

static void ui_cmds_click(GLFWwindow *w, const double x, const double y) {
    GLFWcursorposfun pos = glfwSetCursorPosCallback(w, NULL);
    glfwSetCursorPosCallback(w, pos);
    GLFWmousebuttonfun btn = glfwSetMouseButtonCallback(w, NULL);
    glfwSetMouseButtonCallback(w, btn);
    glfwSetCursorPos(w, x, y); // mouse_button_callback re-reads glfwGetCursorPos (stale on the headless null platform otherwise)
    if (pos) pos(w, x, y);
    if (btn) {
        btn(w, GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
        btn(w, GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE, 0);
    }
}

static void ui_cmds_run(Client *client, char *line) {
    GLFWwindow *w = client->window;
    char cmd[16] = {0};
    int skip = 0;
    if (sscanf(line, "%15s %n", cmd, &skip) < 1) return;
    char *rest = line + skip;
    double x = 0, y = 0;
    if (!strcmp(cmd, "ui")) {
        ui_cmds_dispatch(rest);
    } else if (!strcmp(cmd, "move") && sscanf(rest, "%lf %lf", &x, &y) == 2) {
        GLFWcursorposfun pos = glfwSetCursorPosCallback(w, NULL);
        glfwSetCursorPosCallback(w, pos);
        if (pos) pos(w, x, y);
    } else if (!strcmp(cmd, "click") && sscanf(rest, "%lf %lf", &x, &y) == 2) {
        ui_cmds_click(w, x, y);
    } else if (!strcmp(cmd, "clickid") || !strcmp(cmd, "rect")) {
        HtmlRect r;
        const HtmlElem e = html_dom_by_id(rest);
        if (!e || !html_dom_get_rect(e, &r)) {
            fprintf(stderr, "[ui_cmds] #%s: no box\n", rest);
            return;
        }
        fprintf(stderr, "[ui_cmds] #%s rect %.1f %.1f %.1f %.1f\n", rest, r.x, r.y, r.w, r.h);
        if (cmd[0] == 'c') ui_cmds_click(w, r.x + r.w * 0.5, r.y + r.h * 0.5);
    } else if (!strcmp(cmd, "scroll") && sscanf(rest, "%lf", &y) == 1) {
        GLFWscrollfun sc = glfwSetScrollCallback(w, NULL);
        glfwSetScrollCallback(w, sc);
        if (sc) sc(w, 0, y);
    } else if (!strcmp(cmd, "key")) {
        int key = 0;
        char act[16] = "";
        if (sscanf(rest, "%d %15s", &key, act) < 1) return;
        GLFWkeyfun kf = glfwSetKeyCallback(w, NULL);
        glfwSetKeyCallback(w, kf);
        if (!kf) return;
        if (strcmp(act, "release")) kf(w, key, 0, GLFW_PRESS, 0);
        if (strcmp(act, "press")) kf(w, key, 0, GLFW_RELEASE, 0);
    } else if (!strcmp(cmd, "text")) {
        GLFWcharfun cf = glfwSetCharCallback(w, NULL);
        glfwSetCharCallback(w, cf);
        // "\s" sends a space (the line's leading whitespace is skipped above)
        for (const unsigned char *p = (const unsigned char *) rest; cf && *p; p++) {
            if (p[0] == '\\' && p[1] == 's') {
                cf(w, ' ');
                p++;
            } else {
                cf(w, *p);
            }
        }
    } else if (!strcmp(cmd, "focus")) { // focus <id>: element.focus() (headless has no real cursor)
        const HtmlElem e = strchr("#.[", rest[0]) ? html_dom_query(0, rest) : html_dom_by_id(rest); // id or selector
        if (e) html_dom_focus(e);
        fprintf(stderr, "[ui_cmds] focus %s -> %d\n", rest, e != 0);
    } else if (!strcmp(cmd, "value")) { // value <id>: print the field's value (or the focused one for "-")
        char v[512] = "";
        const HtmlElem e = !strcmp(rest, "-") ? html_dom_active_element()
                           : strchr("#.[", rest[0]) ? html_dom_query(0, rest) : html_dom_by_id(rest);
        if (e) html_dom_get_value(e, v, sizeof(v));
        fprintf(stderr, "[ui_cmds] value %s = '%s'%s\n", rest, v, e ? "" : " (no element)");
    } else if (!strcmp(cmd, "at") && sscanf(rest, "%lf %lf", &x, &y) == 2) { // at x y: elementFromPoint chain
        float vw, vh, dpr;
        html_dom_viewport(&vw, &vh, &dpr);
        for (HtmlElem e = html_dom_element_at((float) (x / (dpr > 0 ? dpr : 1)), (float) (y / (dpr > 0 ? dpr : 1))); e; e = html_dom_parent(e)) {
            char tag[32] = "", id[64] = "", cls[96] = "";
            html_dom_tag(e, tag, sizeof(tag));
            html_dom_get_attr(e, "id", id, sizeof(id));
            html_dom_get_attr(e, "class", cls, sizeof(cls));
            fprintf(stderr, "[ui_cmds] at %.0f,%.0f: <%s id='%s' class='%s'>\n", x, y, tag, id, cls);
        }
    } else if (!strcmp(cmd, "con")) {
        ui_console_exec(rest);
    } else if (!strcmp(cmd, "shot")) {
        const char *saved = g_capture.screenshot;
        g_capture.screenshot = rest;
        debug_capture_write(client);
        g_capture.screenshot = saved;
    } else if (!strcmp(cmd, "quit")) {
        glfwSetWindowShouldClose(w, GLFW_TRUE);
    } else {
        fprintf(stderr, "[ui_cmds] unknown command '%s'\n", cmd);
    }
}

static void ui_cmds_poll(Client *client) {
    static const char *path;
    static int checked;
    if (!checked) {
        checked = 1;
        path = getenv("KRUNKNATIVE_UI_CMDS");
        if (path && !*path) path = NULL;
    }
    // One line per frame, and 4 frames after anything but a shot, so a shot queued right
    // behind a command captures its result (layout, transitions' first frames).
    if (!path || g_ui_cmds.cooldown-- > 0) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char line[sizeof(g_ui_cmds.pending)];
    if (fseek(f, g_ui_cmds.offset, SEEK_SET) == 0 && fgets(line, sizeof(line), f)) {
        const size_t len = strlen(line);
        if (len && line[len - 1] == '\n') { // complete lines only
            g_ui_cmds.offset += (long) len;
            line[len - 1] = 0;
            if (line[0]) {
                ui_cmds_run(client, line);
                if (strncmp(line, "shot ", 5)) g_ui_cmds.cooldown = 4;
            }
        }
    }
    fclose(f);
}
