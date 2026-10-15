// Map editor: offscreen project renders and detached 3D views.
//
// ed_render_project() draws one project (its scene, sky, grid and helpers) from any camera
// into an offscreen framebuffer of the main GL context and reads it back. The bus "render"
// tool (editor_bus_link.c) uses it, so renders come from the live editor's renderer, of
// any open project, shown or not.
//
// Detached views (File > Projects > Detach View, natively): an extra GLFW window showing one
// project from its own fly camera. Every GL object of the editor (meshes, VAOs, textures,
// shaders, the HTML page) lives in the main window's context, and VAOs/FBOs are not shared
// between GL contexts, so a view is drawn in the main context into a texture (textures ARE
// shared: the view window is created with the main window as its share context), then the
// view's own context only draws that texture to its window (one VAO + the blit program).
// Views share the one main loop (rendered from the editor's page tick, after the bus) and
// have no HTML page: the in-house HTML engine keeps one document per process (html_ui.h),
// so the panels stay in the main window. Input in a view: right mouse drag looks, WASD/QE
// fly (Shift sprint), wheel moves along the view, left click selects (its project becomes
// the shown one in the main window, so the panels follow), Delete removes, Ctrl+Z / Ctrl+Y,
// Ctrl+S saves, Escape or Ctrl+W closes the view. Focusing a view shows its project.
// Headless (EGL pbuffer, no GLFW contexts) views stay offscreen: they render, nothing is
// presented. A whole second editor with panels is another process: "Open in New Window"
// starts `client_bin --editor --mcp-bus <file>`, which joins the editor bus.
#include "editor_internal.h"
#include <headless.h>
#include <frame_target.h>
#include <sky.h>
#include <render_settings.h> // client_window_centered_pos: a view opens centred, placed before it is shown
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <spawn.h>
#include <unistd.h>
extern char **environ;
#endif

// ---------------------------------------------------------------------------
// offscreen targets (main context)

typedef struct {
    unsigned int fbo, color, depth;
    int w, h;
} EdTarget;

static int target_ensure(EdTarget *t, const int w, const int h) {
    if (t->fbo && t->w == w && t->h == h) return 1;
    if (t->fbo) {
        glDeleteFramebuffers(1, &t->fbo);
        glDeleteTextures(1, &t->color);
        glDeleteRenderbuffers(1, &t->depth);
    }
    memset(t, 0, sizeof(*t));
    glGenFramebuffers(1, &t->fbo);
    glGenTextures(1, &t->color);
    glGenRenderbuffers(1, &t->depth);
    glBindTexture(GL_TEXTURE_2D, t->color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, t->depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t->depth);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, frame_target_framebuffer()); // the window (frame_target.h)
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[editor] offscreen framebuffer %dx%d incomplete (0x%x)\n", w, h, status);
        return 0;
    }
    t->w = w;
    t->h = h;
    return 1;
}

static void target_free(EdTarget *t) {
    if (t->fbo) {
        glDeleteFramebuffers(1, &t->fbo);
        glDeleteTextures(1, &t->color);
        glDeleteRenderbuffers(1, &t->depth);
    }
    memset(t, 0, sizeof(*t));
}

static EdTarget g_render_target; // bus renders

// Draws project p into the bound framebuffer (viewport w x h) from cam (position, fov,
// near/far; rotation from yaw/pitch). The main context must be current.
static void draw_project(EditorProject *p, const Camera *cam, const float yaw, const float pitch, const float ortho_half_h, const int w,
                         const int h, const int flags) {
    EditorProject *prev;
    ed_project_enter(p, &prev);
    if (g_ed.lighting_owner != p) editor_apply_lighting();
    // the project's camera and the view rectangle stand in for the page's 3D view
    const Camera saved_cam = p->camera;
    const float saved_yaw = p->yaw, saved_pitch = p->pitch;
    const int sx = g_ed.view_x, sy = g_ed.view_y, sw = g_ed.view_w, sh = g_ed.view_h, sfw = g_ed.fb_w, sfh = g_ed.fb_h;
    const int fog = g_render_lighting.fog_enabled;
    p->camera = *cam;
    p->yaw = yaw;
    p->pitch = pitch;
    g_ed.view_x = g_ed.view_y = 0;
    g_ed.view_w = g_ed.fb_w = w;
    g_ed.view_h = g_ed.fb_h = h;
    if (flags & ED_RENDER_NO_FOG) g_render_lighting.fog_enabled = 0;

    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    const vec4 sky = g_render_lighting.sky_color;
    glClearColor(sky.x, sky.y, sky.z, 1.0f);
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    p->camera.rotation = (vec3) {pitch, yaw, 0.0f};
    p->camera.ortho = flags & ED_RENDER_ORTHO ? (ortho_half_h > 0 ? ortho_half_h : 100.0f) : 0.0f;
    camera_update_projection_matrix(&p->camera, (float) w / (float) h);
    camera_update_world_inverse_matrix(&p->camera);
    if (!(flags & ED_RENDER_ORTHO)) sky_render(&p->camera);
    if (!(flags & ED_RENDER_NO_HELPERS)) ed_render_grid();
    scene_render(p->scene, &p->camera);
    if (!(flags & ED_RENDER_NO_HELPERS)) ed_render_overlays();

    g_render_lighting.fog_enabled = fog;
    p->camera = saved_cam;
    p->yaw = saved_yaw;
    p->pitch = saved_pitch;
    g_ed.view_x = sx;
    g_ed.view_y = sy;
    g_ed.view_w = sw;
    g_ed.view_h = sh;
    g_ed.fb_w = sfw;
    g_ed.fb_h = sfh;
    ed_project_leave(prev);
}

unsigned char *ed_render_project(EditorProject *p, const Camera *cam, const float yaw, const float pitch, const float ortho_half_h, const int w,
                                 const int h, const int flags) {
    if (!p || w <= 0 || h <= 0 || w > 8192 || h > 8192) return NULL;
    if (!target_ensure(&g_render_target, w, h)) return NULL;
    gl_release_texture(g_render_target.color); // never sampled while drawn into (WebGL / ANGLE feedback loop)
    glBindFramebuffer(GL_FRAMEBUFFER, g_render_target.fbo);
    draw_project(p, cam, yaw, pitch, ortho_half_h, w, h, flags);
    unsigned char *px = malloc((size_t) w * (size_t) h * 4);
    if (px) {
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        // GL rows go bottom-up
        const size_t row = (size_t) w * 4;
        unsigned char *tmp = malloc(row);
        for (int y = 0; tmp && y < h / 2; y++) {
            memcpy(tmp, px + (size_t) y * row, row);
            memcpy(px + (size_t) y * row, px + (size_t) (h - 1 - y) * row, row);
            memcpy(px + (size_t) (h - 1 - y) * row, tmp, row);
        }
        free(tmp);
        for (size_t i = 3; i < (size_t) w * h * 4; i += 4) px[i] = 255;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, frame_target_framebuffer()); // the window (frame_target.h)
    glViewport(0, 0, g_ed.fb_w > 0 ? g_ed.fb_w : w, g_ed.fb_h > 0 ? g_ed.fb_h : h);
    return px;
}

// ---------------------------------------------------------------------------
// detached views

#define ED_MAX_VIEWS 8

typedef struct {
    GLFWwindow *win;       // NULL: offscreen (headless)
    int project_id;
    Camera cam;
    float yaw, pitch;
    EdTarget target;
    unsigned int vao;      // in the view's context
    int looking;
    double mx, my, look_x, look_y;
    int fwd, back, left, right, up, down, sprint;
    double last_click;
} EdView;

static EdView g_views[ED_MAX_VIEWS];
static int g_view_count;
static unsigned int g_blit_program; // shared between contexts (programs are shared objects)

int ed_view_count(void) { return g_view_count; }

static EdView *view_of(GLFWwindow *w) {
    for (int i = 0; i < g_view_count; i++) if (g_views[i].win == w) return &g_views[i];
    return NULL;
}

#ifndef __EMSCRIPTEN__
static unsigned int blit_program(void) {
    if (g_blit_program) return g_blit_program;
#ifdef KN_GLES
    const char *ver = "#version 300 es\nprecision mediump float;\n";
#elif defined(__APPLE__)
    const char *ver = "#version 410 core\n";
#else
    const char *ver = "#version 450 core\n";
#endif
    const char *vs = "out vec2 uv;\nvoid main() {\n"
                     "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
                     "  uv = p;\n  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n}\n";
    const char *fs = "in vec2 uv;\nout vec4 color;\nuniform sampler2D tex;\n"
                     "void main() { color = vec4(texture(tex, uv).rgb, 1.0); }\n";
    const char *vsrc[2] = {ver, vs}, *fsrc[2] = {ver, fs};
    const unsigned int v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(v, 2, vsrc, NULL);
    glShaderSource(f, 2, fsrc, NULL);
    glCompileShader(v);
    glCompileShader(f);
    const unsigned int prog = glCreateProgram();
    glAttachShader(prog, v);
    glAttachShader(prog, f);
    glLinkProgram(prog);
    glDeleteShader(v);
    glDeleteShader(f);
    int ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "[editor] view blit program: %s\n", log);
        glDeleteProgram(prog);
        return 0;
    }
    g_blit_program = prog;
    return prog;
}

static void cursor_fb(GLFWwindow *w, double *x, double *y) {
    int ww, wh, fw, fh;
    glfwGetWindowSize(w, &ww, &wh);
    glfwGetFramebufferSize(w, &fw, &fh);
    if (ww > 0 && wh > 0) {
        *x *= (double) fw / ww;
        *y *= (double) fh / wh;
    }
}

static void view_dirs(const EdView *v, vec3 *f, vec3 *r, vec3 *u) {
    const float cy = cosf(v->yaw), sy = sinf(v->yaw), cp = cosf(v->pitch), sp = sinf(v->pitch);
    *r = (vec3) {cy, 0, -sy};
    *u = (vec3) {sy * sp, cp, cy * sp};
    *f = (vec3) {-sy * cp, sp, -cy * cp};
}

// runs fn with the project's camera and the editor's view rect replaced by this view's, so
// the page's picking code (ed_pick) works on the view's pixels
static int view_pick(EdView *v, EditorProject *p, const double x, const double y, EditorObject **out) {
    int fw = v->target.w, fh = v->target.h;
    if (fw <= 0 || fh <= 0) return 0;
    EditorProject *prev;
    ed_project_enter(p, &prev);
    const Camera saved = p->camera;
    const float sy = p->yaw, sp = p->pitch;
    const int vx = g_ed.view_x, vy = g_ed.view_y, vw = g_ed.view_w, vh = g_ed.view_h, bw = g_ed.fb_w, bh = g_ed.fb_h;
    p->camera = v->cam;
    p->yaw = v->yaw;
    p->pitch = v->pitch;
    g_ed.view_x = g_ed.view_y = 0;
    g_ed.view_w = g_ed.fb_w = fw;
    g_ed.view_h = g_ed.fb_h = fh;
    const int hit = ed_pick(x, y, 0, out, NULL, NULL);
    p->camera = saved;
    p->yaw = sy;
    p->pitch = sp;
    g_ed.view_x = vx;
    g_ed.view_y = vy;
    g_ed.view_w = vw;
    g_ed.view_h = vh;
    g_ed.fb_w = bw;
    g_ed.fb_h = bh;
    ed_project_leave(prev);
    return hit;
}

static void view_close(EdView *v);

static void cb_cursor(GLFWwindow *w, double x, double y) {
    EdView *v = view_of(w);
    if (!v) return;
    cursor_fb(w, &x, &y);
    if (v->looking) {
        v->yaw -= (float) (x - v->mx) * 0.002f;
        v->pitch -= (float) (y - v->my) * 0.002f;
        const float lim = (float) M_PI / 2.0f;
        v->pitch = fmaxf(-lim, fminf(lim, v->pitch));
    }
    v->mx = x;
    v->my = y;
}

static void cb_mouse(GLFWwindow *w, const int button, const int action, const int mods) {
    (void) mods;
    EdView *v = view_of(w);
    if (!v) return;
    EditorProject *p = ed_project_find(v->project_id);
    if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        v->looking = action == GLFW_PRESS;
        glfwSetInputMode(w, GLFW_CURSOR, v->looking && editor_setting_bool("pointerLock") ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
        return;
    }
    if (button != GLFW_MOUSE_BUTTON_LEFT || action != GLFW_PRESS || !p) return;
    EditorObject *o = NULL;
    const int hit = view_pick(v, p, v->mx, v->my, &o);
    if (p != g_ed.shown) ed_project_activate(p);
    ed_obj_select(hit ? o : NULL);
    const double t = glfwGetTime();
    if (hit && t - v->last_click < 0.35) {
        // double click frames the object from this view
        float m[16];
        ed_obj_world_box(o, m);
        const vec3 s = ed_obj_vec(o, "s", (vec3) {1, 1, 1});
        const float r = sqrtf(s.x * s.x + s.y * s.y + s.z * s.z) / 2.0f;
        vec3 f, rr, u;
        view_dirs(v, &f, &rr, &u);
        v->cam.position = (vec3) {m[3] - f.x * r * 2, m[7] - f.y * r * 2, m[11] - f.z * r * 2};
    }
    v->last_click = t;
}

static void cb_scroll(GLFWwindow *w, const double dx, const double dy) {
    (void) dx;
    EdView *v = view_of(w);
    if (!v) return;
    vec3 f, r, u;
    view_dirs(v, &f, &r, &u);
    const float step = (float) editor_setting_num("speedNormal", 70) * (1.0f / 60.0f) * 5.0f * (dy > 0 ? 1.0f : -1.0f);
    v->cam.position.x += f.x * step;
    v->cam.position.y += f.y * step;
    v->cam.position.z += f.z * step;
}

static void cb_key(GLFWwindow *w, const int key, const int scancode, const int action, const int mods) {
    (void) scancode;
    EdView *v = view_of(w);
    if (!v) return;
    const int down = action != GLFW_RELEASE;
    switch (key) {
        case GLFW_KEY_W: case GLFW_KEY_UP: v->fwd = down; break;
        case GLFW_KEY_S: case GLFW_KEY_DOWN: if (!(mods & GLFW_MOD_CONTROL)) v->back = down; break;
        case GLFW_KEY_A: case GLFW_KEY_LEFT: v->left = down; break;
        case GLFW_KEY_D: case GLFW_KEY_RIGHT: v->right = down; break;
        case GLFW_KEY_E: v->up = down; break;
        case GLFW_KEY_Q: v->down = down; break;
        case GLFW_KEY_LEFT_SHIFT: case GLFW_KEY_RIGHT_SHIFT: v->sprint = down; break;
        default: break;
    }
    if (action != GLFW_PRESS) return;
    EditorProject *p = ed_project_find(v->project_id);
    const int ctrl = (mods & GLFW_MOD_CONTROL) != 0;
    if (key == GLFW_KEY_ESCAPE || (ctrl && key == GLFW_KEY_W)) {
        glfwSetWindowShouldClose(w, GLFW_TRUE);
        return;
    }
    if (!p) return;
    EditorProject *prev;
    if (ctrl && (key == GLFW_KEY_Z || key == GLFW_KEY_Y)) {
        ed_project_enter(p, &prev);
        ed_history_poll();
        ed_history_step(key == GLFW_KEY_Y || (mods & GLFW_MOD_SHIFT));
        ed_project_leave(prev);
    } else if (ctrl && key == GLFW_KEY_S) {
        ed_project_save_ui(p);
    } else if (key == GLFW_KEY_DELETE || key == GLFW_KEY_BACKSPACE) {
        ed_project_enter(p, &prev);
        if (p->selected) ed_obj_remove(p->selected, 0, 1);
        ed_project_leave(prev);
    }
}

static void cb_focus(GLFWwindow *w, const int focused) {
    EdView *v = view_of(w);
    if (!v || !focused) return;
    EditorProject *p = ed_project_find(v->project_id);
    if (p && p != g_ed.shown) ed_project_activate(p); // the main window's panels follow
}

static void set_context_hints(void) {
#if defined(KRUNKNATIVE_GL_ANGLE)
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_EGL_CONTEXT_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#elif defined(__APPLE__)
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#else
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
#endif
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE); // shown once placed (ed_view_open): never moved after it maps
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
}
#endif

void ed_view_open(EditorProject *p) {
    if (!p) return;
#ifdef __EMSCRIPTEN__
    editor_message("Detached views are native only; use Open in New Tab.");
#else
    if (g_view_count >= ED_MAX_VIEWS) {
        editor_message("Too many detached views.");
        return;
    }
    EdView *v = &g_views[g_view_count];
    memset(v, 0, sizeof(*v));
    v->project_id = p->id;
    v->cam = p->camera;
    v->yaw = p->yaw;
    v->pitch = p->pitch;
    GLFWwindow *main = g_ed.client ? g_ed.client->window : NULL;
    if (!headless_enabled() && main) {
        set_context_hints();
        char title[200];
        snprintf(title, sizeof(title), "%s - view", ed_project_title(p));
        v->win = glfwCreateWindow(800, 600, title, NULL, main); // `main` shares its objects with the view
        if (!v->win) {
            editor_message("Could not open a view window.");
            return;
        }
        // centred on the work area of the editor window's monitor, placed while hidden, then shown
        int mx = 0, my = 0, mw = 0, mh = 0, x = 0, y = 0;
        glfwGetWindowPos(main, &mx, &my);
        glfwGetWindowSize(main, &mw, &mh);
        if (client_window_centered_pos(v->win, 800, 600, 1, mx + mw / 2, my + mh / 2, &x, &y)) glfwSetWindowPos(v->win, x, y);
        glfwShowWindow(v->win);
        glfwMakeContextCurrent(v->win);
        glfwSwapInterval(0); // views never hold up the shared loop
        glGenVertexArrays(1, &v->vao); // VAOs are per context
        glfwMakeContextCurrent(main);
        glfwSetCursorPosCallback(v->win, cb_cursor);
        glfwSetMouseButtonCallback(v->win, cb_mouse);
        glfwSetScrollCallback(v->win, cb_scroll);
        glfwSetKeyCallback(v->win, cb_key);
        glfwSetWindowFocusCallback(v->win, cb_focus);
    }
    g_view_count++;
    fprintf(stderr, "[editor] view %d of project %d (%s)\n", g_view_count, p->id, v->win ? "window" : "offscreen");
#endif
}

static void view_close(EdView *v) {
#ifndef __EMSCRIPTEN__
    target_free(&v->target);
    if (v->win) {
        GLFWwindow *main = g_ed.client ? g_ed.client->window : NULL;
        glfwMakeContextCurrent(v->win);
        if (v->vao) glDeleteVertexArrays(1, &v->vao);
        if (main) glfwMakeContextCurrent(main);
        glfwDestroyWindow(v->win);
    }
#endif
    const int i = (int) (v - g_views);
    memmove(&g_views[i], &g_views[i + 1], (size_t) (g_view_count - i - 1) * sizeof(EdView));
    g_view_count--;
}

void ed_view_project_closed(EditorProject *p) {
    for (int i = g_view_count - 1; i >= 0; i--) {
        if (g_views[i].project_id == p->id) view_close(&g_views[i]);
    }
}

void ed_views_fini(void) {
    while (g_view_count) view_close(&g_views[g_view_count - 1]);
#ifndef __EMSCRIPTEN__
    if (g_blit_program) glDeleteProgram(g_blit_program);
#endif
    g_blit_program = 0;
    target_free(&g_render_target);
}

void ed_view_frame(const float now, const float dt) {
    (void) now;
#ifdef __EMSCRIPTEN__
    (void) dt;
#else
    GLFWwindow *main = g_ed.client ? g_ed.client->window : NULL;
    for (int i = g_view_count - 1; i >= 0; i--) {
        EdView *v = &g_views[i];
        EditorProject *p = ed_project_find(v->project_id);
        if (!p || (v->win && glfwWindowShouldClose(v->win))) {
            view_close(v);
            continue;
        }
        // fly
        float x = (float) (v->right - v->left), y = (float) (v->up - v->down), z = (float) (v->back - v->fwd);
        if (x || y || z) {
            const float speed = (float) editor_setting_num(v->sprint ? "speedSprint" : "speedNormal", v->sprint ? 180 : 70) * fminf(dt, 0.1f);
            vec3 f, r, u;
            view_dirs(v, &f, &r, &u);
            v->cam.position.x += (r.x * x + u.x * y - f.x * z) * speed;
            v->cam.position.y += (r.y * x + u.y * y - f.y * z) * speed;
            v->cam.position.z += (r.z * x + u.z * y - f.z * z) * speed;
        }
        int fw = 800, fh = 600;
        if (v->win) glfwGetFramebufferSize(v->win, &fw, &fh);
        if (fw <= 0 || fh <= 0) continue; // minimised
        if (!target_ensure(&v->target, fw, fh)) continue;
        gl_release_texture(v->target.color); // never sampled while drawn into (WebGL / ANGLE feedback loop)
        glBindFramebuffer(GL_FRAMEBUFFER, v->target.fbo);
        draw_project(p, &v->cam, v->yaw, v->pitch, 0, fw, fh, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, frame_target_framebuffer()); // the window (frame_target.h)
        if (!v->win || !main) continue;
        const unsigned int prog = blit_program();
        glFlush(); // the texture is complete before the other context samples it
        glfwMakeContextCurrent(v->win);
        glViewport(0, 0, fw, fh);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glDisable(GL_SCISSOR_TEST);
        if (prog) {
            glUseProgram(prog);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, v->target.color);
            glUniform1i(glGetUniformLocation(prog, "tex"), 0);
            glBindVertexArray(v->vao);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glBindVertexArray(0);
        }
        glfwSwapBuffers(v->win);
        glfwMakeContextCurrent(main);
    }
    if (g_ed.fb_w > 0) glViewport(0, 0, g_ed.fb_w, g_ed.fb_h);
#endif
}

// ---------------------------------------------------------------------------
// a whole editor window for a project: another process on the bus

void ed_view_new_process(EditorProject *p) {
    if (!p) return;
#ifdef __EMSCRIPTEN__
    // web: the map goes to a new tab through storage (editor.html?handoff=1, editor.c)
    EditorProject *prev;
    ed_project_enter(p, &prev);
    char *text = ed_map_export_string();
    ed_project_leave(prev);
    if (!text) return;
    html_storage_set("knEditorHandoff", text);
    free(text);
    EM_ASM({ try { (window.knRealOpen || window.open)("editor.html?handoff=1", "_blank"); } catch (e) {} });
#else
    if (!p->path[0] || p->dirty) {
        editor_message("Save the map to a file first; the new window opens the file.");
        return;
    }
    if (!ed_bus_active()) ed_bus_toggle(); // the windows talk over the editor bus
    char exe[4096] = "";
#ifdef _WIN32
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "\"%s\" --windowed --editor --mcp-bus \"%s\"", exe, p->path);
    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof(si);
    const int ok = CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    if (ok) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
#else
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) exe[n] = 0;
    else snprintf(exe, sizeof(exe), "client_bin");
    char *argv[] = {exe, "--windowed", "--editor", "--mcp-bus", p->path, NULL};
    pid_t pid = 0;
    const int ok = posix_spawn(&pid, exe, NULL, NULL, argv, environ) == 0;
#endif
    if (!ok) {
        editor_message("Could not start a new editor window.");
        return;
    }
    fprintf(stderr, "[editor] project %s moved to a new editor window\n", p->path);
    if (g_ed.project_count > 1) ed_project_close(p, 1);
#endif
}
