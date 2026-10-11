// KrunkNative map editor: Krunker v3.1.1 public/editor.html + js/editor.js as an app page
// (app_mode.h). This file: the AppPage hooks, the frame (3D view inside #container), the
// free-fly camera (PointerLockControls, right mouse), keyboard shortcuts
// (Editor.registerEvents / initControls), the canvas buttons (transform type / space /
// face / group selection, delete, duplicate, quick add), windows and scaleUI.
//
// Parity notes (differences from v3.1.1, intentional):
// - Krunker opens Test Map / Host Map in a new tab; here the client switches to the game
//   page and runs the embedded local server on the exported map (app_test_map).
// - alert()/prompt()/confirm() have no native equivalent: messages go to #chatList like the
//   editor's own warnings, imports read the clipboard / a file, confirms are skipped.
// - Asset browser, mods, publishing and GLTF export need the Krunker API: not available.
// - Holding CTRL disables snapping (the Controls window documents it; v3.1.1 never wired it).
#include "editor_internal.h"
#include <app_mode.h>
#include <gl.h>
#include <html_ui.h>
#include <math.h>
#include <sky.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIN_WIDTH 1920.0f
#define MIN_HEIGHT 937.0f

void ed_gui_number_drag(double fb_y, int released);
void ed_gui_pickr_drag(double fb_x, double fb_y, int released);
int ed_gui_pickr_contains(double fb_x, double fb_y);

// ---------------------------------------------------------------------------
// settings (Editor.initSettings, stored as "kro_editor")

static const char k_default_settings[] =
    "{\"degToRad\":false,\"antiAlias\":false,\"gridVisibility\":true,\"gridOpacity\":0.25,\"gridSize\":250,"
    "\"gridSpacing\":10,\"dbVisibility\":false,\"dbOpacity\":0.5,\"objectHighlight\":false,\"selectThroughInvis\":true,"
    "\"mergeVoxels\":false,\"speedNormal\":70,\"speedSprint\":180,\"voxelSize\":10,\"imageSize\":1,\"imageDepth\":1,"
    "\"imageCol\":true,\"assetAutoGroup\":true,\"preserveFolderState\":true,\"translationSnapping\":1,"
    "\"rotationSnapping\":1,\"scaleSnapping\":1,\"autoBackup\":0,\"quickGUI\":false,\"quickGUIOpen\":1,\"resMltV\":0.6,"
    "\"createObjectNear\":false,\"pointerLock\":true,\"historyLimit\":1000,\"disableHistory\":false,"
    "\"disableWarnings\":false,\"showRealHitbox\":false,\"uiScale\":1,\"borderVisuals\":true,\"fowardKey\":\"W\","
    "\"backwardKey\":\"S\",\"leftKey\":\"A\",\"rightKey\":\"D\",\"upKey\":\"E\",\"downKey\":\"Q\",\"faceModeKey\":\"M\","
    "\"groupModeKey\":\"N\",\"darkMode\":true}";

const char *editor_default_settings(void) { return k_default_settings; }

static void settings_load(void) {
    cJSON_Delete(g_ed.settings);
    g_ed.settings = cJSON_Parse(k_default_settings);
    const int n = html_storage_get("kro_editor", NULL, 0);
    if (n <= 0) return;
    char *buf = malloc((size_t) n + 1);
    if (!buf) return;
    html_storage_get("kro_editor", buf, (size_t) n + 1);
    cJSON *saved = cJSON_Parse(buf);
    free(buf);
    if (!cJSON_IsObject(saved)) {
        cJSON_Delete(saved);
        return;
    }
    cJSON *it;
    cJSON_ArrayForEach(it, saved) {
        cJSON_DeleteItemFromObjectCaseSensitive(g_ed.settings, it->string);
        cJSON_AddItemToObject(g_ed.settings, it->string, cJSON_Duplicate(it, 1));
    }
    cJSON_Delete(saved);
}

void editor_settings_save(void) {
    char *text = cJSON_PrintUnformatted(g_ed.settings);
    if (text) {
        html_storage_set("kro_editor", text);
        free(text);
    }
    // updateTheme: the "dark" stylesheet (editor_dark.css)
    HtmlElem links[8];
    const int n = html_dom_query_all(0, "link[title=dark]", links, 8);
    for (int i = 0; i < n; i++) {
        if (editor_setting_bool("darkMode")) html_dom_remove_attr(links[i], "disabled");
        else html_dom_set_attr(links[i], "disabled", "");
    }
}

double editor_setting_num(const char *key, const double fallback) { return ed_json_num(g_ed.settings, key, fallback); }
int editor_setting_bool(const char *key) { return ed_json_truthy(cJSON_GetObjectItemCaseSensitive(g_ed.settings, key)); }

// ---------------------------------------------------------------------------
// lighting (render.js initScene with the editor's mapConfig; same formulas as main.c)

static vec4 config_color(const char *key, const vec4 fallback) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(g_ed.map_config, key);
    vec4 c = fallback;
    if (cJSON_IsNumber(v)) c = hex_to_vec((int) cJSON_GetNumberValue(v));
    else if (cJSON_IsString(v)) parse_hex_color(v->valuestring, &c);
    return c;
}

void editor_apply_lighting(void) {
    RenderLighting *l = &g_render_lighting;
    const vec4 black = {0, 0, 0, 1};
    l->ambient_color = config_color("ambient", black);
    l->ambient_intensity = (float) ed_json_num(g_ed.map_config, "ambientI", 1.0);
    l->light_color = config_color("light", black);
    l->light_intensity = (float) ed_json_num(g_ed.map_config, "lightI", 1.3);
    l->sky_color = config_color("sky", (vec4) {0.863f, 0.910f, 0.929f, 1});
    l->fog_color = config_color("fog", (vec4) {1, 1, 1, 1});
    l->fog_near = 1.0f;
    l->fog_far = (float) ed_json_num(g_ed.map_config, "fogD", 2000);
    l->fog_enabled = l->fog_far > l->fog_near;
    l->enabled = 1;
    const float u = (float) M_PI * ((float) ed_json_num(g_ed.map_config, "sunAngY", 54) / -180.0f);
    const float v = (float) M_PI * ((float) ed_json_num(g_ed.map_config, "sunAngX", 90) / -180.0f);
    vec3 d = {cosf(v), sinf(v) * sinf(u), sinf(v) * cosf(u)};
    const float len = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    if (len > 0) d = (vec3) {d.x / len, d.y / len, d.z / len};
    l->light_direction = d;
    // the sky dome reads the same keys (skyDome, skyDomeCol0..2, ...) as a map does
    sky_load(ed_json_truthy(cJSON_GetObjectItemCaseSensitive(g_ed.map_config, "skyDome")) ? g_ed.map_config : NULL);
}

// ---------------------------------------------------------------------------
// page state helpers

void editor_message(const char *text) {
    // doWarning-style line in #chatList (Krunker would alert())
    const HtmlElem list = html_dom_by_id("chatList");
    fprintf(stderr, "[editor] %s\n", text);
    if (!list) return;
    EdBuf b;
    edb_init(&b);
    edb_add(&b, "<div class='kn-editor-msg'><div class='chatItem'><i class='material-icons' style='color: #40C4FF;font-size: 20px;transform: translateY(-2px);'>info</i><span class='chatMsg'>");
    edb_add_escaped(&b, text);
    edb_add(&b, "</span></div></div>");
    html_dom_insert_html(list, HTML_INSERT_BEFORE_END, b.buf);
    edb_free(&b);
    // keep the last few
    while (html_dom_child_count(list) > 4) html_dom_remove(html_dom_first_child(list));
}

// doWarning(id, on, text): one #chatList line per warning id
static void do_warning(const char *id, const int on, const char *text) {
    char eid[64];
    snprintf(eid, sizeof(eid), "warning_%s", id);
    const HtmlElem cur = html_dom_by_id(eid);
    if (cur && !on) html_dom_remove(cur);
    if (!on) return;
    if (cur) {
        html_dom_set_text(html_dom_query(cur, ".chatMsg"), text);
        return;
    }
    EdBuf b;
    edb_init(&b);
    edb_addf(&b, "<div id='%s'><div class='chatItem'><i class='material-icons' style='color: #E85656;font-size: 20px;transform: translateY(-2px);'>warning</i><span class='chatMsg'>", eid);
    edb_add_escaped(&b, text);
    edb_add(&b, "</span></div></div>");
    html_dom_insert_html(html_dom_by_id("chatList"), HTML_INSERT_BEFORE_END, b.buf);
    edb_free(&b);
}

static void warning_check_objects(void) {
    // warningCheck("objects"): config.objectColLimit / objectLimit and the verified/premium (F) limits
    if (editor_setting_bool("disableWarnings")) return;
    const cJSON *limits = editor_data_get("limits");
    const int col_limit = (int) ed_json_num(limits, "objectColLimit", 3500), limit = (int) ed_json_num(limits, "objectLimit", 5000);
    const int col_limit_f = 6000, limit_f = 7500; // config.objectColLimitF / objectLimitF
    const int col = g_ed.obj_count[1], all = g_ed.obj_count[0] + g_ed.obj_count[1];
    char text[160];
    snprintf(text, sizeof(text), "Collidable Object limit of %d exceeded (Current: %d)", col_limit, col);
    do_warning("objects_col", col > col_limit, text);
    snprintf(text, sizeof(text), "Verified/Premium Collidable Object limit of %d exceeded (Current: %d)", col_limit_f, col);
    do_warning("objectsF_col", col > col_limit_f, text);
    snprintf(text, sizeof(text), "Object limit of %d exceeded (Current: %d)", limit, all);
    do_warning("objects", all > limit, text);
    snprintf(text, sizeof(text), "Verified/Premium Object limit of %d exceeded (Current: %d)", limit_f, all);
    do_warning("objectsF", all > limit_f, text);
}

void editor_update_object_count(void) {
    warning_check_objects();
    char buf[32];
    const cJSON *limits = editor_data_get("limits");
    snprintf(buf, sizeof(buf), "%d / %d", g_ed.obj_count[2], (int) ed_json_num(limits, "spawnLimit", 20));
    html_dom_set_text(html_dom_by_id("spawnCount"), buf);
    snprintf(buf, sizeof(buf), "%d", g_ed.obj_count[1]);
    html_dom_set_text(html_dom_by_id("objectCount"), buf);
    snprintf(buf, sizeof(buf), "%d", g_ed.obj_count[0]);
    html_dom_set_text(html_dom_by_id("objectNoColCount"), buf);
}

void editor_set_transform_type(const int type) {
    static const char *ids[3] = {"translateOption", "rotateOption", "scaleOption"};
    g_ed.transform_type = type;
    for (int i = 0; i < 3; i++) html_dom_set_class(html_dom_by_id(ids[i]), "selected", i == type);
    if (type == ED_SCALE) editor_set_transform_space(ED_SPACE_LOCAL);
}

void editor_set_transform_space(const int space) {
    if (g_ed.transform_type == ED_SCALE && space == ED_SPACE_WORLD) return;
    g_ed.transform_space = space;
    html_dom_set_html(html_dom_by_id("spaceOption"), space == ED_SPACE_WORLD ? "World" : "Local");
}

static void option_toggle(const char *id, const char *label, const int on) {
    const HtmlElem e = html_dom_by_id(id);
    char text[64];
    snprintf(text, sizeof(text), "%s: %s", label, on ? "ON" : "OFF");
    html_dom_set_html(e, text);
    html_dom_set_style(e, "background-color", on ? "rgba(0, 0, 0, 0.7)" : "");
}

void editor_update_face_selection(void) { option_toggle("faceOption", "Face Selection", g_ed.face_selection); }

void editor_update_group_selection(void) {
    option_toggle("groupOption", "Group Selection", g_ed.group_selection);
    if (!g_ed.group_selection) {
        for (size_t i = 0; i < g_ed.count; i++) g_ed.objects[i]->highlighted = 0;
        if (!g_ed.group_selection) g_ed.pending_count = 0;
    }
}

void editor_toggle_menu(const int open) {
    const char *d = open ? "block" : "none";
    html_dom_set_display(html_dom_by_id("menuHolder"), d);
    html_dom_set_display(html_dom_by_id("menuHider"), d);
    html_dom_set_display(html_dom_by_id("instructionHolder"), d);
    g_ed.menu_open = open;
}

void editor_show_window(const int index) {
    // window.showWindow(e): KE.toggleWindow(false); toggleMenu(true); toggle(false); gen; toggleWindow(true)
    static const char *headers[9] = {"", "Controls", "Assets", "Settings", "Quick Add", "Mods", "Quick Add", "Textures", "Welcome"};
    if (index < 1 || index > 8) return;
    editor_toggle_menu(1);
    g_ed.window_index = index;
    ed_window_show(index);
    html_dom_set_style(g_ed.el_menu_window, "width", index == 1 || index == 3 ? "600px" : "");
    html_dom_set_style(g_ed.el_menu_window, "overflow-y", "auto");
    html_dom_set_html(html_dom_by_id("windowHeader"), headers[index]);
    html_dom_set_display(g_ed.el_window_holder, "block");
}

void editor_close_window(void) {
    html_dom_set_display(g_ed.el_window_holder, "none");
    editor_toggle_menu(0);
    g_ed.window_index = -1;
    ed_gui_color_hide();
}

void editor_frame_object(EditorObject *o) {
    // dblclick: camera to the box centre + view backwards * 2 * bounding sphere radius
    float m[16];
    ed_obj_world_box(o, m);
    const vec3 c = {m[3], m[7], m[11]};
    const vec3 s = ed_obj_vec(o, "s", (vec3) {1, 1, 1});
    const float r = sqrtf(s.x * s.x + s.y * s.y + s.z * s.z) / 2.0f;
    vec3 f;
    editor_camera_dir(&f, NULL, NULL);
    g_ed.camera.position = (vec3) {c.x - f.x * r * 2, c.y - f.y * r * 2, c.z - f.z * r * 2};
}

vec3 editor_near_position(void) {
    // getNearPosition: camera position + (0, -5, -30) rotated by the camera, rounded
    vec3 f, r, u;
    editor_camera_dir(&f, &r, &u);
    const vec3 p = g_ed.camera.position;
    return (vec3) {roundf(p.x - u.x * 5 + f.x * 30), roundf(p.y - u.y * 5 + f.y * 30), roundf(p.z - u.z * 5 + f.z * 30)};
}

void editor_test_map(const int host) {
    // testMap(e): saveVal(e ? "custToLoad" : "custMap", getMapExport()); open "/" in a new tab
    ed_map_store();
    char *text = ed_map_export_string();
    if (!text) return;
    html_storage_set(host ? "custToLoad" : "custMap", text);
#ifdef APP_HAVE_TEST_MAP
    cJSON *map = cJSON_Parse(text);
    free(text);
    if (map && !cJSON_GetObjectItemCaseSensitive(map, "camPos")) {
        // KrunkNative difference: without a Camera Position object v3.1.1 puts the menu
        // camera at [0, 0, 0] (usually inside the floor: a flat fog-grey menu). For Test
        // Map only, use the editor's camera instead; exports stay as Krunker writes them.
        cJSON *cam = cJSON_CreateArray();
        cJSON_AddItemToArray(cam, cJSON_CreateNumber(ed_round(g_ed.camera.position.x, 2)));
        cJSON_AddItemToArray(cam, cJSON_CreateNumber(ed_round(g_ed.camera.position.y, 2)));
        cJSON_AddItemToArray(cam, cJSON_CreateNumber(ed_round(g_ed.camera.position.z, 2)));
        cJSON_AddItemToObject(map, "camPos", cam);
    }
    if (map) app_test_map(g_ed.client, map, APP_MODE_EDITOR);
#else
    free(text);
    editor_message("Map saved for testing (custMap); switching to the game.");
    app_set_mode(g_ed.client, APP_MODE_GAME, NULL);
#endif
}

// ---------------------------------------------------------------------------
// layout: scaleUI() and the #container rectangle in framebuffer pixels

static void scale_ui(void) {
    float vw = 0, vh = 0;
    html_dom_viewport(&vw, &vh, NULL);
    if (vw <= 0 || vh <= 0) return;
    float scale = fminf(vh / MIN_HEIGHT, vw / MIN_WIDTH);
    scale = roundf(scale * 1000.0f) / 1000.0f * (float) editor_setting_num("uiScale", 1); // toFixed(3) * uiScale
    if (scale <= 0) scale = 1;
    g_ed.elem_scale = scale;
    const HtmlElem base = html_dom_by_id("uiBase");
    char v[64];
    snprintf(v, sizeof(v), "scale(%g)", scale);
    html_dom_set_style(base, "transform", v);
    snprintf(v, sizeof(v), "%.3fpx", vw / scale);
    html_dom_set_style(base, "width", v);
    snprintf(v, sizeof(v), "%.3fpx", vh / scale);
    html_dom_set_style(base, "height", v);
}

static void update_view_rect(void) {
    float vw = 0, vh = 0;
    html_dom_viewport(&vw, &vh, NULL);
    int fw = 0, fh = 0;
    glfwGetFramebufferSize(g_ed.client->window, &fw, &fh);
    g_ed.fb_w = fw;
    g_ed.fb_h = fh;
    g_ed.css_to_fb = vw > 0 ? (float) fw / vw : 1.0f;
    HtmlRect r;
    if (!g_ed.el_container || !html_dom_get_rect(g_ed.el_container, &r) || r.w <= 0 || r.h <= 0) {
        g_ed.view_w = g_ed.view_h = 0;
        return;
    }
    const float s = g_ed.css_to_fb;
    // the container is clipped by the window; GL wants the bottom-left origin
    const float x0 = fmaxf(r.x, 0) * s, y0 = fmaxf(r.y, 0) * s;
    const float x1 = fminf(r.x + r.w, vw) * s, y1 = fminf(r.y + r.h, vh) * s;
    g_ed.view_x = (int) roundf(x0);
    g_ed.view_w = (int) roundf(x1 - x0);
    g_ed.view_h = (int) roundf(y1 - y0);
    g_ed.view_y = fh - (int) roundf(y0) - g_ed.view_h;
}

static int in_view(const double x, const double y) {
    const double top = g_ed.fb_h - g_ed.view_y - g_ed.view_h;
    return x >= g_ed.view_x && x < g_ed.view_x + g_ed.view_w && y >= top && y < top + g_ed.view_h;
}

// is the point over the bare 3D area (not over a panel, toolbar menu or canvas button)?
static int over_canvas(const double x, const double y) {
    if (!in_view(x, y)) return 0;
    if (g_ed.menu_open) return 0;
    const float s = g_ed.css_to_fb > 0 ? g_ed.css_to_fb : 1;
    const HtmlElem e = html_dom_element_at((float) (x / s), (float) (y / s));
    if (!e) return 1;
    // web: the WebGL #canvas under the page is what elementFromPoint finds there
    return e == g_ed.el_container || e == g_ed.el_center || e == html_dom_by_id("canvasUI") || e == html_dom_by_id("view") || e == html_dom_by_id("canvas") ||
           e == html_dom_by_id("uiBase") || e == html_dom_body() || e == html_dom_root() || e == html_dom_by_id("editorUI");
}

static int is_typing(void) {
    const HtmlElem e = html_dom_active_element();
    if (!e) return 0;
    char tag[16] = "", type[16] = "";
    html_dom_tag(e, tag, sizeof(tag));
    if (!strcmp(tag, "textarea")) return 1;
    if (strcmp(tag, "input")) return 0;
    if (html_dom_get_attr(e, "type", type, sizeof(type)) < 0) return 1; // text by default
    return !strcmp(type, "text") || !strcmp(type, "number");
}

// ---------------------------------------------------------------------------
// handlers (inline on* names of editor.html and of the generated markup)

static int h_close_window(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) { (void) u; (void) ev; (void) a; (void) n; editor_close_window(); return 1; }

static int h_show_window(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u; (void) ev;
    if (n >= 1 && a[0].kind == HTML_ARG_NUM) editor_show_window((int) a[0].num);
    return 1;
}

static int h_test_map(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u; (void) ev;
    editor_test_map(n >= 1 && ((a[0].kind == HTML_ARG_BOOL || a[0].kind == HTML_ARG_NUM) && a[0].num != 0));
    return 1;
}

static void relayout_center(void) {
    // Resizable.centerCallback: #center fills what the visible panels leave
    const HtmlElem center = g_ed.el_center;
    HtmlElem panels[2] = {html_dom_query(0, ".left.panel"), html_dom_query(0, ".right.panel")};
    const float lw = g_ed.panel_hidden[0] ? 0 : html_dom_offset_width(panels[0]);
    const float rw = g_ed.panel_hidden[1] ? 0 : html_dom_offset_width(panels[1]);
    const float bw = html_dom_offset_width(html_dom_by_id("uiBase"));
    char v[32];
    snprintf(v, sizeof(v), "%dpx", (int) lw);
    html_dom_set_style(center, "left", v);
    snprintf(v, sizeof(v), "%dpx", (int) (bw - lw - rw));
    html_dom_set_style(center, "width", v);
    html_dom_set_style(html_dom_by_id("canvasUI"), "left", g_ed.panel_hidden[0] ? "43px" : "0px");
    html_dom_set_style(html_dom_by_id("canvasInfo"), "right", g_ed.panel_hidden[1] ? "43px" : "0px");
}

static int h_resizable_hide(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u; (void) a; (void) n;
    // [, , ,].fill(1): the panel is three parents up from the button
    HtmlElem p = ev->target;
    for (int i = 0; i < 3 && p; i++) p = html_dom_parent(p);
    if (!p) return 1;
    const int right = html_dom_has_class(p, "right");
    html_dom_set_display(p, "none");
    g_ed.panel_hidden[right] = 1;
    relayout_center();
    return 1;
}

static int h_resizable_restore(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u; (void) a; (void) n;
    // the panel is the restore button's next sibling
    const HtmlElem panel = html_dom_next(ev->target);
    if (!panel) return 1;
    const int right = html_dom_has_class(panel, "right");
    html_dom_set_display(panel, "");
    g_ed.panel_hidden[right] = 0;
    relayout_center();
    return 1;
}

static int h_transform(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u; (void) ev;
    const int which = n >= 1 ? (int) a[0].num : 0;
    switch (which) {
        case 0: case 1: case 2: editor_set_transform_type(which); break;
        case 3: editor_set_transform_space(g_ed.transform_space == ED_SPACE_WORLD ? ED_SPACE_LOCAL : ED_SPACE_WORLD); break;
        case 4: g_ed.face_selection = !g_ed.face_selection; editor_update_face_selection(); break;
        case 5: g_ed.group_selection = !g_ed.group_selection; editor_update_group_selection(); break;
        case 6: editor_show_window(4); break;
        case 7: ed_obj_delete_selected(); break;
        case 8: ed_obj_duplicate(); break;
        default: break;
    }
    return 1;
}

static int h_noop(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) { (void) u; (void) ev; (void) a; (void) n; return 1; }

static int h_fallback(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u; (void) ev; (void) a; (void) n;
    const char *name = html_ui_current_handler();
    if (name) fprintf(stderr, "[editor] unhandled %s\n", name);
    return 1;
}

static int h_keydown(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u; (void) a; (void) n;
    if (html_key_is_escape(ev->key)) {
        if (g_ed.window_index > 0) editor_close_window();
        return 0;
    }
    return 1;
}

static void install_handlers(void) {
    static HtmlHandlerEntry table[96];
    size_t count = 0;
    const HtmlHandlerEntry own[] = {
        {"closeWindow", h_close_window},
        {"showWindow", h_show_window},
        {"KE.testMap", h_test_map},
        {"KE.resizable.hide", h_resizable_hide},
        {"KE.resizable.restore", h_resizable_restore},
        {"KE.kn.option", h_transform},
        {"openPub", h_noop},
    };
    for (size_t i = 0; i < sizeof(own) / sizeof(own[0]); i++) table[count++] = own[i];
    ed_gui_install(table, &count, sizeof(table) / sizeof(table[0]));
    ed_panels_install(table, &count, sizeof(table) / sizeof(table[0]));
    html_ui_set_handlers(table, count, NULL, h_fallback);
    html_ui_set_listener(NULL, "keydown", h_keydown);
}

static void wire_canvas_buttons(void) {
    // initControls: click listeners on the canvas buttons -> inline handlers here
    static const struct { const char *id; int which; } buttons[] = {
        {"translateOption", 0}, {"rotateOption", 1}, {"scaleOption", 2}, {"spaceOption", 3},
        {"faceOption", 4}, {"groupOption", 5}, {"canvasAdd", 6}, {"deleteOption", 7}, {"duplicateOption", 8},
    };
    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
        char v[48];
        snprintf(v, sizeof(v), "KE.kn.option(%d)", buttons[i].which);
        html_dom_set_attr(html_dom_by_id(buttons[i].id), "onclick", v);
    }
}

// ---------------------------------------------------------------------------
// AppPage hooks

static void page_enter(Client *client, const char *query) {
    memset(&g_ed, 0, sizeof(g_ed));
    g_ed.client = client;
    g_ed.active = 1;
    g_ed.window_index = -1;
    g_ed.snapping = 1;
    g_ed.scene = scene_init();
    g_ed.camera.zoom = 1.0f;
    g_ed.camera.fov = 70.0f * (float) M_PI / 180.0f; // PerspectiveCamera(70, aspect, 1, 10000)
    g_ed.camera.near = 1.0f;
    g_ed.camera.far = 10000.0f;
    g_ed.camera.position = (vec3) {0, 50, 100};
    g_ed.el_container = html_dom_by_id("container");
    g_ed.el_center = html_dom_by_id("center");
    g_ed.el_menu_window = html_dom_by_id("menuWindow");
    g_ed.el_window_holder = html_dom_by_id("windowHolder");
    g_ed.el_left_list = html_dom_query(html_dom_query(0, ".left.panel"), ".list");
    g_ed.el_right_list = html_dom_query(html_dom_query(0, ".right.panel"), ".list");

    settings_load();
    g_ed.advanced = cJSON_Parse("{\"rotation\":0,\"texture\":\"DEFAULT\",\"scaleMapX\":1,\"scaleMapY\":1,\"scaleMapZ\":1,"
                                "\"reflectDir\":0,\"frameThickness\":10,\"breakableHealth\":1}");
    ed_map_defaults();
    install_handlers();
    wire_canvas_buttons();

    char version[32];
    snprintf(version, sizeof(version), "Krunker Editor v%g", 0.7);
    html_dom_set_text(html_dom_by_id("versionTxt"), version);
    editor_toggle_menu(0);

    // the map: ?map=<name> (assets/maps), ?new, else the autosave (knEditorMap / mapToEdit)
    const char *map = query ? strstr(query, "map=") : NULL;
    int loaded = 0;
    if (map) {
        char name[128];
        size_t i = 0;
        for (map += 4; *map && *map != '&' && i + 1 < sizeof(name); map++) name[i++] = *map;
        name[i] = 0;
        char rel[192];
        snprintf(rel, sizeof(rel), "maps/%s%s", name, strstr(name, ".json") ? "" : ".json");
        char *path = concat(assets_path(), rel);
        unsigned char *buf = NULL;
        size_t len = 0;
        if (path && ensure_asset_file(path) && !read_file(path, &buf, &len)) {
            char *text = malloc(len + 1);
            if (text) {
                memcpy(text, buf, len);
                text[len] = 0;
                loaded = ed_map_import(text);
                free(text);
            }
            free(buf);
        }
        free(path);
    }
    if (!loaded && !(query && strstr(query, "new"))) loaded = ed_map_restore();
    if (!loaded) editor_apply_lighting();

    ed_toolbar_show();
    ed_panel_left_show();
    ed_panel_right_blank();
    editor_set_transform_type(0);
    editor_set_transform_space(ED_SPACE_WORLD);
    editor_update_face_selection();
    editor_update_group_selection();
    editor_settings_save(); // theme
    editor_update_object_count();
    scale_ui();
    relayout_center();
    fprintf(stderr, "[editor] entered (query \"%s\"), %zu objects\n", query ? query : "", g_ed.count);
}

static void page_leave(Client *client) {
    (void) client;
    if (!g_ed.active) return;
    ed_map_store();
    if (g_ed.looking) glfwSetInputMode(g_ed.client->window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    ed_objects_clear();
    ed_history_clear();
    ed_render_fini();
    free(g_ed.scene ? g_ed.scene->meshes : NULL);
    free(g_ed.scene);
    cJSON_Delete(g_ed.map_config);
    cJSON_Delete(g_ed.game_config);
    cJSON_Delete(g_ed.server_config);
    cJSON_Delete(g_ed.settings);
    cJSON_Delete(g_ed.advanced);
    cJSON_Delete(g_ed.extra_map);
    cJSON_Delete(g_ed.copy);
    free(g_ed.pending_group);
    free(g_ed.undos);
    free(g_ed.redos);
    sky_load(NULL);
    memset(&g_ed, 0, sizeof(g_ed));
}

static void move_camera(const float dt) {
    // Editor.render: (x, y, z) in camera space * speed * dt, applied with the camera quaternion
    float x = 0, y = 0, z = 0;
    if (g_ed.move_forward) z -= 1;
    if (g_ed.move_backward) z += 1;
    if (g_ed.move_left) x -= 1;
    if (g_ed.move_right) x += 1;
    if (g_ed.move_up) y += 1;
    if (g_ed.move_down) y -= 1;
    if (!x && !y && !z) return;
    const float speed = (float) editor_setting_num(g_ed.move_sprint ? "speedSprint" : "speedNormal", g_ed.move_sprint ? 180 : 70);
    vec3 f, r, u;
    editor_camera_dir(&f, &r, &u);
    vec3 *p = &g_ed.camera.position;
    p->x += (r.x * x + u.x * y - f.x * z) * speed * dt;
    p->y += (r.y * x + u.y * y - f.y * z) * speed * dt;
    p->z += (r.z * x + u.z * y - f.z * z) * speed * dt;
}

static void page_tick(Client *client, const float now, const float dt) {
    (void) client;
    if (!g_ed.active) return;
    g_ed.now = now;
    update_view_rect();
    move_camera(fminf(dt, 0.1f));
    if (now >= g_ed.history_next) {
        ed_history_poll(); // initHistory: once a second
        g_ed.history_next = now + 1.0;
    }
    ed_groups_sync();
    if (g_ed.dirty_panel) {
        g_ed.dirty_panel = 0;
        if (g_ed.selected) ed_panel_right_object();
        else if (!strcmp(ed_panel_right_active(), "object")) ed_panel_right_blank();
    }
    if (g_ed.dirty_counts) {
        g_ed.dirty_counts = 0;
        editor_update_object_count();
    }
    if (g_ed.dirty_storage && now >= g_ed.storage_next) {
        g_ed.dirty_storage = 0;
        g_ed.storage_next = now + 2.0;
        ed_map_store();
    }

    // body: rgba(0, 0, 0, 0.7) over the browser's white page outside the panels
    glClearColor(0.302f, 0.302f, 0.302f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (g_ed.view_w <= 0 || g_ed.view_h <= 0) return;

    glViewport(g_ed.view_x, g_ed.view_y, g_ed.view_w, g_ed.view_h);
    glEnable(GL_SCISSOR_TEST);
    glScissor(g_ed.view_x, g_ed.view_y, g_ed.view_w, g_ed.view_h);
    const vec4 sky = g_render_lighting.sky_color;
    glClearColor(sky.x, sky.y, sky.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    g_ed.camera.rotation = (vec3) {g_ed.pitch, g_ed.yaw, 0.0f};
    camera_update_projection_matrix(&g_ed.camera, (float) g_ed.view_w / (float) g_ed.view_h);
    camera_update_world_inverse_matrix(&g_ed.camera);
    for (size_t i = 0; i < g_ed.count; i++) {
        Object *o = g_ed.objects[i]->render_map && g_ed.objects[i]->render_map->object_count ? g_ed.objects[i]->render_map->objects[0] : NULL;
        if (o) client_animate_object_texture(o, now);
    }
    sky_tick(dt * 1000.0f);
    sky_render(&g_ed.camera);
    ed_render_grid();
    scene_render(g_ed.scene, &g_ed.camera);
    ed_render_overlays();

    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, g_ed.fb_w, g_ed.fb_h);
}

static void start_look(const int on) {
    g_ed.looking = on;
    if (editor_setting_bool("pointerLock")) glfwSetInputMode(g_ed.client->window, GLFW_CURSOR, on ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    g_ed.look_last_x = g_ed.mouse_x;
    g_ed.look_last_y = g_ed.mouse_y;
}

static void canvas_click(const double x, const double y) {
    // container mousedown, left button: face toggle / group pick / select / deselect
    if (g_ed.selected) {
        const int axis = ed_gizmo_hit(x, y);
        if (axis) {
            ed_gizmo_begin(x, y, axis);
            return;
        }
    }
    EditorObject *o = NULL;
    int face = 0;
    vec3 hit;
    if (!ed_pick(x, y, (g_ed.mods & GLFW_MOD_ALT) != 0, &o, &hit, &face)) {
        ed_obj_select(NULL); // hideTransform
        return;
    }
    if (g_ed.face_selection && editor_prefab_has(o->prefab, EP_EDITFACES)) {
        cJSON *faces = ed_prop_get(o, "faces");
        const cJSON *cur = cJSON_GetArrayItem(faces, face);
        cJSON_ReplaceItemInArray(faces, face, cJSON_CreateBool(!cJSON_IsTrue(cur)));
        ed_prop_set(o, "faces", faces);
        cJSON_Delete(faces);
        ed_obj_rebuild(o);
        g_ed.dirty_panel = 1;
        return;
    }
    if (g_ed.group_selection) {
        size_t i = 0;
        for (; i < g_ed.pending_count; i++) if (g_ed.pending_group[i] == o->uuid) break;
        if (i < g_ed.pending_count) {
            g_ed.pending_group[i] = g_ed.pending_group[--g_ed.pending_count];
            o->highlighted = 0;
        } else {
            if (g_ed.pending_count == g_ed.pending_cap) {
                const size_t cap = g_ed.pending_cap ? g_ed.pending_cap * 2 : 32;
                uint32_t *n = realloc(g_ed.pending_group, cap * sizeof(uint32_t));
                if (!n) return;
                g_ed.pending_group = n;
                g_ed.pending_cap = cap;
            }
            g_ed.pending_group[g_ed.pending_count++] = o->uuid;
            o->highlighted = 1;
        }
    }
    ed_obj_select(o);
}

static int page_mouse_button(Client *client, const int button, const int action, const int mods, const double x, const double y) {
    (void) client;
    g_ed.mods = mods;
    g_ed.mouse_x = x;
    g_ed.mouse_y = y;
    if (action == GLFW_RELEASE) {
        ed_gui_number_drag(y, 1);
        ed_gui_pickr_drag(x, y, 1);
        if (button == GLFW_MOUSE_BUTTON_RIGHT && g_ed.looking) {
            start_look(0);
            return 1;
        }
        if (button == GLFW_MOUSE_BUTTON_LEFT && g_ed.gizmo.active) {
            ed_gizmo_end();
            return 1;
        }
        return 0;
    }
    if (!over_canvas(x, y)) {
        // the colour picker hides on any click outside it and its input
        if (action == GLFW_PRESS && !ed_gui_pickr_contains(x, y)) {
            const float s = g_ed.css_to_fb > 0 ? g_ed.css_to_fb : 1;
            extern int ed_gui_pickr_target(HtmlElem);
            if (!ed_gui_pickr_target(html_dom_element_at((float) (x / s), (float) (y / s)))) ed_gui_color_hide();
        }
        return 0;
    }
    ed_gui_color_hide();
    // a click on the 3D view takes focus away from the panel inputs
    const HtmlElem active = html_dom_active_element();
    if (active) html_dom_blur(active);
    if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        start_look(1);
        return 1;
    }
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        const double t = glfwGetTime();
        const int dbl = t - g_ed.last_click_time < 0.35 && fabs(x - g_ed.last_click_x) < 6 && fabs(y - g_ed.last_click_y) < 6;
        g_ed.last_click_time = t;
        g_ed.last_click_x = x;
        g_ed.last_click_y = y;
        if (dbl && !g_ed.face_selection && !g_ed.group_selection) {
            EditorObject *o = NULL;
            if (ed_pick(x, y, (mods & GLFW_MOD_ALT) != 0, &o, NULL, NULL)) editor_frame_object(o);
            return 1;
        }
        canvas_click(x, y);
        return 1;
    }
    return 1;
}

static int page_cursor(Client *client, const double x, const double y) {
    (void) client;
    const double dx = x - g_ed.mouse_x, dy = y - g_ed.mouse_y;
    g_ed.mouse_x = x;
    g_ed.mouse_y = y;
    ed_gui_number_drag(y, 0);
    ed_gui_pickr_drag(x, y, 0);
    if (g_ed.looking) {
        // PointerLockControls: movementX/Y * 0.002 (CSS px)
        const float s = g_ed.css_to_fb > 0 ? g_ed.css_to_fb : 1;
        g_ed.yaw -= (float) (dx / s) * 0.002f;
        g_ed.pitch -= (float) (dy / s) * 0.002f;
        const float lim = (float) M_PI / 2.0f;
        g_ed.pitch = fmaxf(-lim, fminf(lim, g_ed.pitch));
        return 1;
    }
    if (g_ed.gizmo.active) {
        ed_gizmo_drag(x, y);
        return 1;
    }
    g_ed.gizmo.hover = g_ed.selected && in_view(x, y) ? ed_gizmo_hit(x, y) : 0;
    return 0;
}

static int page_scroll(Client *client, const double dx, const double dy) {
    (void) client; (void) dx;
    if (!over_canvas(g_ed.mouse_x, g_ed.mouse_y)) return 0;
    // mousewheel: along the view axis, speedNormal * clock.getDelta() * 5
    vec3 f;
    editor_camera_dir(&f, NULL, NULL);
    const float step = (float) editor_setting_num("speedNormal", 70) * (1.0f / 60.0f) * 5.0f * (dy > 0 ? 1.0f : -1.0f);
    g_ed.camera.position.x += f.x * step;
    g_ed.camera.position.y += f.y * step;
    g_ed.camera.position.z += f.z * step;
    return 1;
}

static int key_is(const int key, const char *setting) {
    const char *s = ed_json_str(g_ed.settings, setting, "");
    return s[0] && key == (unsigned char) s[0] && key >= GLFW_KEY_A && key <= GLFW_KEY_Z; // GLFW letters == ASCII
}

static void set_move(const int key, const int mods, const int down) {
    if (key == GLFW_KEY_LEFT_SHIFT || key == GLFW_KEY_RIGHT_SHIFT) g_ed.move_sprint = down;
    if (key_is(key, "fowardKey") || key == GLFW_KEY_UP) g_ed.move_forward = down;
    if (key_is(key, "backwardKey") || key == GLFW_KEY_DOWN) g_ed.move_backward = down;
    if (key_is(key, "leftKey") || key == GLFW_KEY_LEFT) g_ed.move_left = down;
    if (key_is(key, "rightKey") || key == GLFW_KEY_RIGHT) g_ed.move_right = down;
    if (key_is(key, "upKey")) g_ed.move_up = down;
    if (key_is(key, "downKey") && (!(mods & GLFW_MOD_CONTROL) || !down)) g_ed.move_down = down;
}

static int page_key(Client *client, const int key, const int scancode, const int action, const int mods) {
    (void) client; (void) scancode;
    g_ed.mods = mods;
    if (ed_gui_key_capture(key, action)) return 1;
    if (action == GLFW_RELEASE) {
        set_move(key, mods, 0);
        return 0;
    }
    if (key == GLFW_KEY_ESCAPE) {
        if (g_ed.window_index > 0) {
            editor_close_window();
            return 1;
        }
        const HtmlElem active = html_dom_active_element();
        if (active) html_dom_blur(active);
        return 1; // never the game menu
    }
    if (is_typing() || g_ed.menu_open) return 0;
    if (action == GLFW_PRESS || action == GLFW_REPEAT) set_move(key, mods, 1);
    if (action != GLFW_PRESS) return 1;
    if (key_is(key, "faceModeKey")) {
        g_ed.face_selection = !g_ed.face_selection;
        editor_update_face_selection();
    }
    if (key_is(key, "groupModeKey")) {
        g_ed.group_selection = !g_ed.group_selection;
        editor_update_group_selection();
    }
    const int ctrl = (mods & GLFW_MOD_CONTROL) != 0, shift = (mods & GLFW_MOD_SHIFT) != 0;
    switch (key) {
        case GLFW_KEY_1: editor_set_transform_type(0); break;
        case GLFW_KEY_2: editor_set_transform_type(1); break;
        case GLFW_KEY_3: editor_set_transform_type(2); break;
        case GLFW_KEY_GRAVE_ACCENT: editor_set_transform_space(g_ed.transform_space == ED_SPACE_WORLD ? ED_SPACE_LOCAL : ED_SPACE_WORLD); break;
        case GLFW_KEY_BACKSPACE:
        case GLFW_KEY_DELETE: ed_obj_delete_selected(); break;
        case GLFW_KEY_P: ed_obj_create_placeholder(); break;
        case GLFW_KEY_R: if (shift) ed_obj_duplicate(); break;
        case GLFW_KEY_C:
            if (ctrl) ed_obj_copy(0, 0);
            else if (shift) ed_obj_toggle_prop("collidable");
            break;
        case GLFW_KEY_V:
            if (ctrl) ed_obj_paste();
            else if (shift) ed_obj_toggle_prop("visible");
            break;
        case GLFW_KEY_F:
            if (!shift) ed_obj_flip_xz();
            break;
        case GLFW_KEY_G:
            if (shift) {
                if (mods & GLFW_MOD_ALT) ed_group_stop(1);
                else if (g_ed.selected && ed_group_of(g_ed.selected)) ed_group_stop(0);
                else ed_group_create();
            }
            break;
        case GLFW_KEY_Y: if (ctrl) ed_history_execute(1); break;
        case GLFW_KEY_Z: if (ctrl) ed_history_execute(shift); break;
        case GLFW_KEY_X: editor_show_window(4); break;
        default: break;
    }
    return 1;
}

static void page_resize(Client *client, const int w, const int h) {
    (void) client; (void) w; (void) h;
    if (!g_ed.active) return;
    scale_ui();
    relayout_center();
}

static const AppPage k_editor_page = {
    .file = "editor.html",
    .enter = page_enter,
    .leave = page_leave,
    .tick = page_tick,
    .mouse_button = page_mouse_button,
    .cursor = page_cursor,
    .scroll = page_scroll,
    .key = page_key,
    .chr = NULL,
    .resize = page_resize,
};

const AppPage *editor_app_page(void) { return &k_editor_page; }
