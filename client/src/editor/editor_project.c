// Map editor projects: several maps open in one editor page (KrunkNative; Krunker's editor
// edits one map per tab). Each EditorProject (editor_internal.h) owns its map, objects,
// meshes (its own Scene), history, selection, camera, file path and dirty flag; the page,
// panels, input and settings stay in g_ed and always show g_ed.shown.
//
// Switching:
//   ed_project_activate(p)      show p: lighting, panels, counts, tab strip, title
//   ed_project_enter(p, &prev)  work on p without showing it (bus calls, opening files): every
//   ed_project_leave(prev)      ed_* function reads g_ed.proj; panel rebuilds asked for in
//                               between are deferred (g_ed.ui_stale) and the shown project's
//                               lighting comes back on leave
//
// UI: a tab strip at the bottom of the 3D view (#knTabs, shown with two or more projects),
// Ctrl+Tab / Ctrl+Shift+Tab, Ctrl+S (save), Ctrl+Shift+S (save all), Ctrl+O (open),
// Ctrl+Alt+N (new; Ctrl+N stays the browser's), Ctrl+W (close) and File > Projects. Closing a
// project or the window with unsaved changes asks first (Save / Discard / Cancel).
#include "editor_internal.h"
#include "editor_model.h"
#include <app_mode.h>
#include <html_ui.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
#include <unistd.h>
#elif defined(_WIN32)
#include <process.h>
#define getpid _getpid
#endif

// ---------------------------------------------------------------------------
// command line (`client_bin --editor [flags] [maps...]`, app_mode_parse_args)

#define ED_MAX_CLI_FILES 32
static char *g_cli_files[ED_MAX_CLI_FILES];
static int g_cli_file_count;

int editor_cli_file_count(void) { return g_cli_file_count; }
const char *editor_cli_file(const int i) { return i >= 0 && i < g_cli_file_count ? g_cli_files[i] : NULL; }

void editor_cli_add_file(const char *path) {
    if (!path || g_cli_file_count >= ED_MAX_CLI_FILES) return;
    g_cli_files[g_cli_file_count++] = strdup(path);
}

// ---------------------------------------------------------------------------
// lifetime

static EditorProject *project_alloc(void) {
    EditorProject *p = calloc(1, sizeof(EditorProject));
    if (!p) return NULL;
    p->id = ++g_ed.next_project_id;
    p->scene = scene_init();
    p->camera.zoom = 1.0f;
    p->camera.fov = 70.0f * (float) M_PI / 180.0f; // PerspectiveCamera(70, aspect, 1, 10000)
    p->camera.near = 1.0f;
    p->camera.far = 10000.0f;
    p->camera.position = (vec3) {0, 50, 100};
    return p;
}

void ed_project_enter(EditorProject *p, EditorProject **prev) {
    *prev = g_ed.proj;
    g_ed.proj = p;
}

void ed_project_leave(EditorProject *prev) {
    if (g_ed.proj == prev) return;
    g_ed.proj = prev;
    if (!prev || prev != g_ed.shown) return;
    if (g_ed.lighting_owner != prev) editor_apply_lighting();
    if (g_ed.ui_stale) {
        g_ed.ui_stale = 0;
        ed_panel_refresh_all();
        g_ed.dirty_counts = 1;
        g_ed.dirty_panel = 1;
    }
}

int ed_project_background(void) { return g_ed.proj != g_ed.shown; }

static void list_add(EditorProject *p) {
    EditorProject **n = realloc(g_ed.projects, (g_ed.project_count + 1) * sizeof(EditorProject *));
    if (!n) return;
    g_ed.projects = n;
    g_ed.projects[g_ed.project_count++] = p;
    g_ed.dirty_tabs = 1;
}

EditorProject *ed_project_create(void) {
    EditorProject *p = project_alloc();
    if (!p) return NULL;
    EditorProject *prev;
    ed_project_enter(p, &prev);
    ed_map_defaults();
    ed_project_leave(prev);
    list_add(p);
    return p;
}

// frees everything the project owns (its meshes need g_ed.proj == p)
static void project_free(EditorProject *p) {
    EditorProject *prev;
    ed_project_enter(p, &prev);
    ed_objects_clear();
    ed_history_clear();
    ed_terrain_fini();
    cJSON_Delete(p->map_config);
    cJSON_Delete(p->server_config);
    cJSON_Delete(p->extra_map);
    cJSON_Delete(p->gui);
    cJSON_Delete(p->scripts);
    free(p->pending_group);
    free(p->undos);
    free(p->redos);
    if (p->scene) free(p->scene->meshes);
    free(p->scene);
    g_ed.proj = prev == p ? NULL : prev;
    if (g_ed.lighting_owner == p) g_ed.lighting_owner = NULL;
    free(p);
}

void ed_projects_fini(void) {
    while (g_ed.project_count) project_free(g_ed.projects[--g_ed.project_count]);
    free(g_ed.projects);
    g_ed.projects = NULL;
    g_ed.proj = g_ed.shown = NULL;
}

EditorProject *ed_project_find(const int id) {
    for (size_t i = 0; i < g_ed.project_count; i++) {
        if (g_ed.projects[i]->id == id) return g_ed.projects[i];
    }
    return NULL;
}

static int project_index(const EditorProject *p) {
    for (size_t i = 0; i < g_ed.project_count; i++) {
        if (g_ed.projects[i] == p) return (int) i;
    }
    return -1;
}

const char *ed_project_map_name(const EditorProject *p) {
    return ed_json_str(p->map_config, "name", "");
}

const char *ed_project_title(const EditorProject *p) {
    if (p->name[0]) return p->name;
    const char *n = ed_project_map_name(p);
    return n[0] ? n : "Untitled";
}

static const char *base_name(const char *path) {
    const char *b = path;
    for (const char *c = path; *c; c++) if (*c == '/' || *c == '\\') b = c + 1;
    return b;
}

// absolute path of an existing file (realpath / _fullpath); 0 when it cannot be resolved
int ed_abs_path(const char *in, char *out, const size_t n) {
    if (!in || !*in || n < 2) return 0;
#if defined(__EMSCRIPTEN__)
    snprintf(out, n, "%s", in);
    return 1;
#elif defined(_WIN32)
    return _fullpath(out, in, n) != NULL;
#else
    char buf[4096];
    if (!realpath(in, buf)) return 0;
    snprintf(out, n, "%s", buf);
    return 1;
#endif
}

static void set_path(EditorProject *p, const char *path) {
    snprintf(p->path, sizeof(p->path), "%s", path ? path : "");
    // absolute, so the bus can match it from any working directory
    char abs[1024];
    if (path && *path && ed_abs_path(path, abs, sizeof(abs))) snprintf(p->path, sizeof(p->path), "%s", abs);
    if (p->path[0]) {
        snprintf(p->name, sizeof(p->name), "%s", base_name(p->path));
        const size_t n = strlen(p->name);
        if (n > 5 && !strcmp(p->name + n - 5, ".json")) p->name[n - 5] = 0;
    } else {
        p->name[0] = 0;
    }
    g_ed.dirty_tabs = 1;
}

static int ref_match(const char *a, const char *b) {
    // case-insensitive
    for (; *a && *b; a++, b++) {
        const char x = (char) (*a >= 'A' && *a <= 'Z' ? *a + 32 : *a), y = (char) (*b >= 'A' && *b <= 'Z' ? *b + 32 : *b);
        if (x != y) return 0;
    }
    return !*a && !*b;
}

EditorProject *ed_project_resolve(const char *ref) {
    // "1.2" / "2" (local id), a path, a basename with or without .json, or the map name
    if (!ref || !*ref) return g_ed.shown;
    const char *dot = strchr(ref, '.');
    char *end = NULL;
    const long id = strtol(dot && dot[1] >= '0' && dot[1] <= '9' ? dot + 1 : ref, &end, 10);
    if (end && !*end && id > 0) {
        EditorProject *p = ed_project_find((int) id);
        if (p) return p;
    }
    char abs[1024] = "";
    if (!ed_abs_path(ref, abs, sizeof(abs))) abs[0] = 0;
    for (size_t i = 0; i < g_ed.project_count; i++) {
        EditorProject *p = g_ed.projects[i];
        if (p->path[0] && (!strcmp(p->path, ref) || (abs[0] && !strcmp(p->path, abs)))) return p;
    }
    char base[256];
    snprintf(base, sizeof(base), "%s", base_name(ref));
    const size_t n = strlen(base);
    if (n > 5 && !strcmp(base + n - 5, ".json")) base[n - 5] = 0;
    for (size_t i = 0; i < g_ed.project_count; i++) {
        if (g_ed.projects[i]->name[0] && ref_match(g_ed.projects[i]->name, base)) return g_ed.projects[i];
    }
    for (size_t i = 0; i < g_ed.project_count; i++) {
        if (ref_match(ed_project_map_name(g_ed.projects[i]), ref)) return g_ed.projects[i];
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// change tracking

void ed_mark_changed(void) {
    EditorProject *p = g_ed.proj;
    if (p == g_ed.shown) g_ed.dirty_storage = 1; // the autosave keeps the shown map
    if (!p) return;
    p->revision++;
    if (!p->dirty) {
        p->dirty = 1;
        g_ed.dirty_tabs = 1;
    }
    ed_bus_notify_change(p);
}

static void mark_clean(EditorProject *p) {
    p->dirty = 0;
    p->saved_revision = p->revision;
    g_ed.dirty_tabs = 1;
}

// ---------------------------------------------------------------------------
// showing a project

static void update_title(void) {
#ifndef __EMSCRIPTEN__
    if (!g_ed.client || !g_ed.client->window || !g_ed.shown) return;
    char title[256];
    snprintf(title, sizeof(title), "Krunker Editor - %s%s", ed_project_title(g_ed.shown), g_ed.shown->dirty ? " *" : "");
    glfwSetWindowTitle(g_ed.client->window, title);
#endif
}

void ed_project_activate(EditorProject *p) {
    if (!p) return;
    if (g_ed.shown == p && g_ed.proj == p) return;
    if (g_ed.shown && g_ed.proj == g_ed.shown) ed_history_poll(); // record the last second's edits where they happened
    // transient input / tool state belongs to the view, not the map
    g_ed.gizmo.active = 0;
    g_ed.gizmo.hover = 0;
    g_ed.move_forward = g_ed.move_backward = g_ed.move_left = g_ed.move_right = g_ed.move_up = g_ed.move_down = 0;
    if (g_ed.looking && g_ed.client && g_ed.client->window) glfwSetInputMode(g_ed.client->window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    g_ed.looking = 0;
    ed_quickgui_hide();
    ed_gui_color_hide();
    g_ed.shown = g_ed.proj = p;
    g_ed.ui_stale = 0;
    editor_apply_lighting();
    if (!p->selected && !strcmp(ed_panel_right_active(), "object")) ed_panel_right_blank();
    ed_panel_refresh_all();
    g_ed.dirty_panel = g_ed.dirty_counts = g_ed.dirty_tabs = 1;
    ed_bus_notify_projects();
}

// ---------------------------------------------------------------------------
// files

static char *read_text(const char *path) {
    unsigned char *buf = NULL;
    size_t len = 0;
    if (read_file(path, &buf, &len)) return NULL;
    char *text = malloc(len + 1);
    if (text) {
        memcpy(text, buf, len);
        text[len] = 0;
    }
    free(buf);
    return text;
}

// a path as given, else assets/maps/<name>[.json]
static char *load_map_text(const char *path, char *resolved, const size_t n) {
    snprintf(resolved, n, "%s", path);
    char *text = read_text(path);
    if (text) return text;
    char rel[512];
    snprintf(rel, sizeof(rel), "maps/%s%s", path, strstr(path, ".json") ? "" : ".json");
    char *full = concat(assets_path(), rel);
    if (full && ensure_asset_file(full)) text = read_text(full);
    if (text) snprintf(resolved, n, "%s", full);
    free(full);
    return text;
}

EditorProject *ed_project_open_text(const char *text, const char *path, const int activate) {
    EditorProject *p = ed_project_create();
    if (!p) return NULL;
    EditorProject *prev;
    ed_project_enter(p, &prev);
    const int ok = ed_map_import(text);
    ed_history_clear();
    ed_project_leave(prev);
    if (!ok) {
        ed_project_close(p, 1);
        return NULL;
    }
    set_path(p, path);
    mark_clean(p);
    if (activate || !g_ed.shown) ed_project_activate(p);
    ed_bus_notify_projects();
    return p;
}

EditorProject *ed_project_open_file(const char *path, const int activate) {
    if (!path || !*path) return NULL;
    // already open: show it
    EditorProject *open = NULL;
    char abs[1024] = "";
    if (ed_abs_path(path, abs, sizeof(abs))) {
        for (size_t i = 0; i < g_ed.project_count && !open; i++) if (!strcmp(g_ed.projects[i]->path, abs)) open = g_ed.projects[i];
    }
    if (open) {
        if (activate) ed_project_activate(open);
        return open;
    }
    char resolved[1024];
    char *text = load_map_text(path, resolved, sizeof(resolved));
    if (!text) {
        char msg[1200];
        snprintf(msg, sizeof(msg), "Cannot read map file %s", path);
        editor_message(msg);
        return NULL;
    }
    EditorProject *p = ed_project_open_text(text, resolved, activate);
    free(text);
    if (p) fprintf(stderr, "[editor] project %d: %s (%zu objects)\n", p->id, p->path, p->count);
    return p;
}

EditorProject *ed_project_new(const int activate) {
    EditorProject *p = ed_project_create();
    if (!p) return NULL;
    mark_clean(p);
    if (activate || !g_ed.shown) ed_project_activate(p);
    ed_bus_notify_projects();
    return p;
}

static int write_text_file(const char *path, const char *text) {
#ifdef __EMSCRIPTEN__
    (void) path;
    (void) text;
    return 0;
#else
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp%d", path, (int) getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    const size_t len = strlen(text);
    const int ok = fwrite(text, 1, len, f) == len;
    if (fclose(f) != 0 || !ok || rename(tmp, path) != 0) {
        remove(tmp);
        return 0;
    }
    return 1;
#endif
}

long ed_project_save(EditorProject *p, const char *path, const int adopt, const int pretty) {
    ed_script_flush(); // the script window's latest edits
    // writes the map JSON (the assets/maps format, getMapExport); returns bytes, -1 on failure
    if (!p) return -1;
    const char *target = path && *path ? path : p->path;
    if (!target[0]) return -1;
    EditorProject *prev;
    ed_project_enter(p, &prev);
    if (p == g_ed.shown) ed_history_poll();
    cJSON *map = ed_map_export(0);
    ed_project_leave(prev);
    char *text = pretty ? cJSON_Print(map) : ed_model_print(map); // compact: JSON.stringify's bytes
    cJSON_Delete(map);
    if (!text) return -1;
    char resolved[1024];
    snprintf(resolved, sizeof(resolved), "%s", target);
    const long bytes = (long) strlen(text);
    const int ok = write_text_file(resolved, text);
    free(text);
    if (!ok) return -1;
    if (!path || !*path || adopt) {
        set_path(p, resolved);
        mark_clean(p);
        ed_bus_notify_projects();
    }
    return bytes;
}

// ---------------------------------------------------------------------------
// closing

int ed_project_close(EditorProject *p, const int force) {
    const int i = project_index(p);
    if (i < 0) return 0;
    if (p->dirty && !force) return 0;
    memmove(&g_ed.projects[i], &g_ed.projects[i + 1], (g_ed.project_count - (size_t) i - 1) * sizeof(EditorProject *));
    g_ed.project_count--;
    const int was_shown = g_ed.shown == p;
    if (was_shown) g_ed.shown = NULL;
    if (g_ed.proj == p) g_ed.proj = NULL;
    ed_view_project_closed(p); // detached views of it close (editor_view.c)
    project_free(p);
    g_ed.dirty_tabs = 1;
    if (!g_ed.project_count) {
        if (g_ed.active) ed_project_new(1); // the editor always has a map
    } else if (was_shown) {
        ed_project_activate(g_ed.projects[i > 0 ? i - 1 : 0]);
    } else if (!g_ed.proj) {
        g_ed.proj = g_ed.shown;
    }
    ed_bus_notify_projects();
    return 1;
}

static void save_prompted(void *user, const char *value) {
    EditorProject *p = ed_project_find((int) (intptr_t) user);
    if (!p || !value || !*value) return;
    char msg[1200];
    if (ed_project_save(p, value, 1, 0) >= 0) snprintf(msg, sizeof(msg), "Saved %s", p->path);
    else snprintf(msg, sizeof(msg), "Could not write %s", value);
    editor_message(msg);
}

// UI save: untitled maps ask for a path (natively), the web downloads the map
void ed_project_save_ui(EditorProject *p) {
    if (!p) return;
#ifdef __EMSCRIPTEN__
    EditorProject *prev;
    ed_project_enter(p, &prev);
    ed_file_export(0);
    ed_project_leave(prev);
    mark_clean(p);
#else
    if (!p->path[0]) {
        char def[1024];
        const char *home = getenv("HOME");
        snprintf(def, sizeof(def), "%s/%s.json", home && *home ? home : ".", ed_project_title(p));
        ed_prompt("Save map as (path)", def, save_prompted, (void *) (intptr_t) p->id);
        return;
    }
    char msg[1200];
    if (ed_project_save(p, NULL, 0, 0) >= 0) snprintf(msg, sizeof(msg), "Saved %s", p->path);
    else snprintf(msg, sizeof(msg), "Could not write %s", p->path);
    editor_message(msg);
#endif
}

void ed_project_save_all(void) {
    for (size_t i = 0; i < g_ed.project_count; i++) {
        EditorProject *p = g_ed.projects[i];
        if (p->dirty && p->path[0]) ed_project_save_ui(p);
    }
}

static void close_choice(void *user, const int choice) {
    EditorProject *p = ed_project_find((int) (intptr_t) user);
    if (!p) return;
    if (choice == 0) { // Save
        if (!p->path[0]) {
            ed_project_save_ui(p);
            return;
        }
        if (ed_project_save(p, NULL, 0, 0) < 0) {
            editor_message("Save failed; the project stays open.");
            return;
        }
        ed_project_close(p, 1);
    } else if (choice == 1) { // Discard
        ed_project_close(p, 1);
    }
}

void ed_project_request_close(EditorProject *p) {
    if (!p) return;
    if (!p->dirty) {
        ed_project_close(p, 1);
        return;
    }
    char msg[512];
    snprintf(msg, sizeof(msg), "\"%s\" has unsaved changes. Save them before closing?", ed_project_title(p));
    static const char *const buttons[] = {"Save", "Discard", "Cancel"};
    ed_choice("Close project", msg, buttons, 3, close_choice, (void *) (intptr_t) p->id);
}

int ed_projects_dirty(void) {
    int n = 0;
    for (size_t i = 0; i < g_ed.project_count; i++) n += g_ed.projects[i]->dirty != 0;
    return n;
}

static void quit_choice(void *user, const int choice) {
    (void) user;
    if (choice == 0) {
        for (size_t i = 0; i < g_ed.project_count; i++) {
            EditorProject *p = g_ed.projects[i];
            if (p->dirty && p->path[0] && ed_project_save(p, NULL, 0, 0) >= 0) continue;
            if (p->dirty) {
                ed_project_activate(p);
                editor_message("This map has no file yet: save it (File > Projects > Save), then close the window again.");
                return;
            }
        }
    } else if (choice != 1) {
        return; // Cancel
    }
    for (size_t i = 0; i < g_ed.project_count; i++) mark_clean(g_ed.projects[i]);
    if (g_ed.client && g_ed.client->window) glfwSetWindowShouldClose(g_ed.client->window, GLFW_TRUE);
}

int ed_project_confirm_quit(void) {
    // window close: 1 = close now; 0 = asked (the answer closes it)
    const int dirty = ed_projects_dirty();
    if (!dirty) return 1;
    char msg[256];
    snprintf(msg, sizeof(msg), "%d project%s unsaved changes.", dirty, dirty == 1 ? " has" : "s have");
    static const char *const buttons[] = {"Save All", "Discard", "Cancel"};
    ed_choice("Quit editor", msg, buttons, 3, quit_choice, NULL);
    return 0;
}

static void open_picked(void *user, const char *path) {
    (void) user;
    if (path && *path) ed_project_open_file(path, 1);
}

void ed_project_open_ui(void) { ed_pick_file(".json,.txt", open_picked, NULL); }

void ed_project_cycle(const int dir) {
    if (g_ed.project_count < 2) return;
    int i = project_index(g_ed.shown);
    i = (i + dir + (int) g_ed.project_count) % (int) g_ed.project_count;
    ed_project_activate(g_ed.projects[i]);
}

// ---------------------------------------------------------------------------
// tab strip (#knTabs at the bottom of #center)

void ed_tabs_render(void) {
    g_ed.dirty_tabs = 0;
    update_title();
    const HtmlElem center = g_ed.el_center;
    if (!center) return;
    HtmlElem bar = html_dom_by_id("knTabs");
    if (g_ed.project_count < 2) {
        if (bar) html_dom_set_display(bar, "none");
        return;
    }
    if (!bar) {
        html_dom_insert_html(center, HTML_INSERT_BEFORE_END, "<div id='knTabs'></div>");
        bar = html_dom_by_id("knTabs");
        if (!bar) return;
    }
    html_dom_set_display(bar, "");
    EdBuf b;
    edb_init(&b);
    for (size_t i = 0; i < g_ed.project_count; i++) {
        const EditorProject *p = g_ed.projects[i];
        edb_addf(&b, "<div class='knTab%s' onclick='KE.kn.tab(%d)' title='", p == g_ed.shown ? " knTabActive" : "", p->id);
        edb_add_escaped(&b, p->path[0] ? p->path : ed_project_title(p));
        edb_add(&b, "'><span class='knTabName'>");
        edb_add_escaped(&b, ed_project_title(p));
        edb_addf(&b, "%s</span><span class='knTabClose' onclick='KE.kn.tabClose(%d)'>&#215;</span></div>", p->dirty ? " *" : "", p->id);
    }
    edb_add(&b, "<div class='knTab knTabNew' onclick='KE.kn.tabNew()' title='New project'>+</div>");
    html_dom_set_html(bar, b.buf ? b.buf : "");
    edb_free(&b);
}

static int h_tab(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u; (void) ev;
    const char *name = html_ui_current_handler();
    const int id = n >= 1 && a[0].kind == HTML_ARG_NUM ? (int) a[0].num : 0;
    if (!name) return 1;
    if (!strcmp(name, "KE.kn.tab")) {
        EditorProject *p = ed_project_find(id);
        if (p) ed_project_activate(p);
    } else if (!strcmp(name, "KE.kn.tabClose")) {
        ed_project_request_close(ed_project_find(id));
        return 0; // not the tab's click as well
    } else if (!strcmp(name, "KE.kn.tabNew")) {
        ed_project_new(1);
    }
    return 1;
}

void ed_project_install(HtmlHandlerEntry *table, size_t *count, const size_t max) {
    const HtmlHandlerEntry entries[] = {
        {"KE.kn.tab", h_tab},
        {"KE.kn.tabClose", h_tab},
        {"KE.kn.tabNew", h_tab},
    };
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]) && *count < max; i++) table[(*count)++] = entries[i];
}

// keyboard: 1 = handled
int ed_project_key(const int key, const int mods) {
    const int ctrl = (mods & GLFW_MOD_CONTROL) != 0, shift = (mods & GLFW_MOD_SHIFT) != 0, alt = (mods & GLFW_MOD_ALT) != 0;
    if (!ctrl) return 0;
    switch (key) {
        case GLFW_KEY_TAB: ed_project_cycle(shift ? -1 : 1); return 1;
        case GLFW_KEY_S:
            if (shift) ed_project_save_all();
            else ed_project_save_ui(g_ed.shown);
            return 1;
        case GLFW_KEY_O: ed_project_open_ui(); return 1;
        case GLFW_KEY_W: ed_project_request_close(g_ed.shown); return 1;
        case GLFW_KEY_N:
            if (alt) {
                ed_project_new(1);
                return 1;
            }
            return 0;
        case GLFW_KEY_PAGE_DOWN: ed_project_cycle(1); return 1;
        case GLFW_KEY_PAGE_UP: ed_project_cycle(-1); return 1;
        default: return 0;
    }
}

// File > Projects menu (editor_panels.c toolbar)
void ed_project_menu(const int action) {
    switch (action) {
        case ED_PM_NEW: ed_project_new(1); break;
        case ED_PM_OPEN: ed_project_open_ui(); break;
        case ED_PM_SAVE: ed_project_save_ui(g_ed.shown); break;
        case ED_PM_SAVE_ALL: ed_project_save_all(); break;
        case ED_PM_CLOSE: ed_project_request_close(g_ed.shown); break;
        case ED_PM_NEXT: ed_project_cycle(1); break;
        case ED_PM_DETACH: ed_view_open(g_ed.shown); break;
        case ED_PM_NEW_WINDOW: ed_view_new_process(g_ed.shown); break;
        case ED_PM_BUS: ed_bus_toggle(); break;
        case ED_PM_ACCESS: ed_bus_manage_access(); break;
        default: break;
    }
}
