// Menu window markup: loads ui/windows/windows.json + the captured window files and
// generates what krunknative.js's WINDOWS[id].gen() wrote into #menuWindow. The JS
// regex substitutions became literal-anchor splices over the same templates
// (tpl_splice below), so the output stays byte-identical to the page's.
#include <ui_logic.h>
#include <backend_ui.h>
#include <map_catalog.h>
#include <ui_mobile.h>
#include <client_settings.h>
#include <download_ui.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- StrBuf

static void sb_reserve(StrBuf *sb, const size_t extra) {
    if (sb->len + extra + 1 <= sb->cap) return;
    size_t cap = sb->cap ? sb->cap : 256;
    while (sb->len + extra + 1 > cap) cap *= 2;
    char *data = realloc(sb->data, cap);
    if (!data) return; // keep the old buffer; the append is dropped
    sb->data = data;
    sb->cap = cap;
}

void sb_append_n(StrBuf *sb, const char *s, const size_t n) {
    if (!s || !n) return;
    sb_reserve(sb, n);
    if (sb->len + n + 1 > sb->cap) return;
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = 0;
}

void sb_append(StrBuf *sb, const char *s) {
    if (s) sb_append_n(sb, s, strlen(s));
}

void sb_append_esc(StrBuf *sb, const char *s) {
    if (!s) return;
    for (; *s; s++) {
        switch (*s) {
            case '&': sb_append(sb, "&amp;"); break;
            case '<': sb_append(sb, "&lt;"); break;
            case '>': sb_append(sb, "&gt;"); break;
            case '"': sb_append(sb, "&quot;"); break;
            case '\'': sb_append(sb, "&#39;"); break;
            default: sb_append_n(sb, s, 1); break;
        }
    }
}

void sb_appendf(StrBuf *sb, const char *fmt, ...) {
    char stack[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(stack, sizeof(stack), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t) n < sizeof(stack)) {
        sb_append_n(sb, stack, (size_t) n);
        return;
    }
    char *heap = malloc((size_t) n + 1);
    if (!heap) return;
    va_start(ap, fmt);
    vsnprintf(heap, (size_t) n + 1, fmt, ap);
    va_end(ap);
    sb_append_n(sb, heap, (size_t) n);
    free(heap);
}

char *sb_finish(StrBuf *sb) {
    char *out = sb->data;
    if (!out) out = calloc(1, 1);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
    return out;
}

void sb_free(StrBuf *sb) {
    free(sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

// ---------------------------------------------------------------- templating
// Cursor-based splicing over a template: copy up to and including `after`, append
// `replacement`, then skip to `until` (which stays in the output). This replaces the JS
// `tpl.replace(/after[^x]*until/, ...)` idiom with literal anchors. When an anchor is
// missing the rest of the template is copied unchanged (like a non-matching regex).

typedef struct {
    const char *cur;
} Tpl;

static void tpl_splice(StrBuf *out, Tpl *t, const char *after, const char *until, const char *replacement, const int escape) {
    const char *a = strstr(t->cur, after);
    if (!a) return;
    const char *end_after = a + strlen(after);
    const char *u = strstr(end_after, until);
    if (!u) return;
    sb_append_n(out, t->cur, (size_t) (end_after - t->cur));
    if (escape) sb_append_esc(out, replacement);
    else sb_append(out, replacement);
    t->cur = u;
}

static void tpl_finish(StrBuf *out, Tpl *t) {
    sb_append(out, t->cur);
    t->cur += strlen(t->cur);
}

// tpl.split(needle).join(replacement)
void sb_append_replace_all(StrBuf *out, const char *tpl, const char *needle, const char *replacement) {
    const size_t nlen = strlen(needle);
    const char *cur = tpl;
    for (;;) {
        const char *hit = nlen ? strstr(cur, needle) : NULL;
        if (!hit) {
            sb_append(out, cur);
            return;
        }
        sb_append_n(out, cur, (size_t) (hit - cur));
        sb_append(out, replacement);
        cur = hit + nlen;
    }
}

// ---------------------------------------------------------------- window files
// ui/windows (tools/ui-import/windows_v7.py): `<n>.html` = the markup v7.1.4 writes for
// showWindow(n), settings-<basic|advanced>-<t>.html = windows[0] per tab, controls-<t>.html
// = windows[6] per tab, windows.json = their header / stickyHeader / #menuWindow class and
// inline style.

#define UI_WINDOWS_MAX 64
#define UI_SETTINGS_TABS_MAX 8
#define UI_CONTROLS_TABS_MAX 8

typedef struct {
    char key[8];
    UiWindowMeta meta;
    char header[48];
    char cls[48];
    char style[96];
    char label[32];
    char *html;      // windows[key].file content
} UiWindowFile;

typedef struct {
    char name[32];
    char *html;
} UiTabFile;

static UiWindowFile g_files[UI_WINDOWS_MAX];
static int g_file_count;
static UiTabFile g_settings_tabs[2][UI_SETTINGS_TABS_MAX];
static int g_settings_tab_count[2];
static UiWindowFile g_settings_file; // meta of windows[0] (header "Game Settings", sticky, dark)
static UiTabFile g_controls_tabs[UI_CONTROLS_TABS_MAX];
static int g_controls_tab_count;
static int g_loaded;

static char *windows_dir(void) {
#ifdef __EMSCRIPTEN__
    return concat("/ui/windows/", "");       // --preload-file ui/windows@/ui/windows
#else
    const char *dir = getenv("KRUNKNATIVE_UI_DIR"); // main.c's ui dir override
    if (dir && *dir) return concat(dir, "/windows/");
    return concat(ui_default_path(), "windows/");
#endif
}

static char *load_window_file(const char *dir, const char *file) {
    if (!file || !*file) return NULL;
    char *path = concat(dir, file);
    unsigned char *data = NULL;
    size_t length = 0;
    if (read_file(path, &data, &length) != 0) {
        fprintf(stderr, "[ui] missing window file %s\n", path);
        free(path);
        return NULL;
    }
    free(path);
    // Strip one trailing newline the generator may have left (the JS strings had none).
    while (length && (data[length - 1] == '\n' || data[length - 1] == '\r')) data[--length] = 0;
    return (char *) data;
}

static const char *json_str(const cJSON *obj, const char *key, const char *fallback) {
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(item) ? cJSON_GetStringValue(item) : fallback;
}

static void read_meta(UiWindowFile *w, const cJSON *entry) {
    snprintf(w->header, sizeof(w->header), "%s", json_str(entry, "header", ""));
    snprintf(w->cls, sizeof(w->cls), "%s", json_str(entry, "cls", ""));
    snprintf(w->style, sizeof(w->style), "%s", json_str(entry, "style", ""));
    snprintf(w->label, sizeof(w->label), "%s", json_str(entry, "label", ""));
    w->meta.header = w->header;
    w->meta.cls = w->cls;
    w->meta.style = w->style;
    w->meta.label = w->label;
    w->meta.sticky = cJSON_IsTrue(cJSON_GetObjectItem(entry, "sticky"));
}

static int load_tabs(const char *dir, const cJSON *list, UiTabFile *out, const int max) {
    int n = 0;
    const cJSON *tab;
    cJSON_ArrayForEach(tab, list) {
        if (n == max) break;
        snprintf(out[n].name, sizeof(out[n].name), "%s", json_str(tab, "name", ""));
        out[n].html = load_window_file(dir, json_str(tab, "file", NULL));
        n++;
    }
    return n;
}

int ui_windows_load(void) {
    if (g_loaded) return 1;
    g_loaded = 1;

    char *dir = windows_dir();
    char *index_path = concat(dir, "windows.json");
    unsigned char *data = NULL;
    size_t length = 0;
    if (read_file(index_path, &data, &length) != 0) {
        fprintf(stderr, "[ui] cannot read %s: windows are unavailable\n", index_path);
        free(index_path);
        free(dir);
        return 0;
    }
    free(index_path);

    cJSON *root = cJSON_Parse((const char *) data);
    free(data);
    if (!root) {
        fprintf(stderr, "[ui] windows.json: parse error\n");
        free(dir);
        return 0;
    }

    const cJSON *entry;
    cJSON_ArrayForEach(entry, cJSON_GetObjectItem(root, "windows")) {
        if (g_file_count == UI_WINDOWS_MAX || !entry->string) continue;
        UiWindowFile *w = &g_files[g_file_count++];
        snprintf(w->key, sizeof(w->key), "%s", entry->string);
        read_meta(w, entry);
        w->html = load_window_file(dir, json_str(entry, "file", NULL));
    }

    const cJSON *settings = cJSON_GetObjectItem(root, "settings");
    read_meta(&g_settings_file, settings);
    snprintf(g_settings_file.key, sizeof(g_settings_file.key), "1");
    g_settings_tab_count[0] = load_tabs(dir, cJSON_GetObjectItem(settings, "basic"), g_settings_tabs[0], UI_SETTINGS_TABS_MAX);
    g_settings_tab_count[1] = load_tabs(dir, cJSON_GetObjectItem(settings, "advanced"), g_settings_tabs[1], UI_SETTINGS_TABS_MAX);
    for (int v = 0; v < 2; v++) {
        for (int t = 0; t < g_settings_tab_count[v]; t++) {
            char *mobile = ui_mobile_settings_html(g_settings_tabs[v][t].html); // isMobile: "mobile" rows shown
            if (mobile) {
                free(g_settings_tabs[v][t].html);
                g_settings_tabs[v][t].html = mobile;
            }
        }
    }
    g_controls_tab_count = load_tabs(dir, cJSON_GetObjectItem(cJSON_GetObjectItem(root, "controls"), "tabs"), g_controls_tabs, UI_CONTROLS_TABS_MAX);

    cJSON_Delete(root);
    free(dir);
    return 1;
}

static const UiWindowFile *find_file(const char *key) {
    for (int i = 0; i < g_file_count; i++) {
        if (!strcmp(g_files[i].key, key)) return &g_files[i];
    }
    return NULL;
}

const char *ui_windows_html(const char *key) {
    ui_windows_load();
    const UiWindowFile *w = find_file(key);
    return w && w->html ? w->html : "";
}

const UiWindowMeta *ui_windows_meta(const char *key) {
    ui_windows_load();
    if (!strcmp(key, "1")) return &g_settings_file.meta;
    const UiWindowFile *w = find_file(key);
    return w ? &w->meta : NULL;
}

static int view_index(const int view) { return view ? 1 : 0; }

int ui_settings_view_tab_count(const int view) {
    ui_windows_load();
    return g_settings_tab_count[view_index(view)];
}

const char *ui_settings_view_tab_html(const int view, const int tab) {
    ui_windows_load();
    const int v = view_index(view);
    return tab >= 0 && tab < g_settings_tab_count[v] && g_settings_tabs[v][tab].html ? g_settings_tabs[v][tab].html : "";
}

const char *ui_settings_view_tab_name(const int view, const int tab) {
    ui_windows_load();
    const int v = view_index(view);
    return tab >= 0 && tab < g_settings_tab_count[v] ? g_settings_tabs[v][tab].name : "";
}

// The advanced view lists every setting (the basic one is a subset).
int ui_settings_tab_count(void) { return ui_settings_view_tab_count(1); }
const char *ui_settings_tab_html(const int tab) { return ui_settings_view_tab_html(1, tab); }

int ui_controls_tab_count(void) {
    ui_windows_load();
    return g_controls_tab_count;
}

const char *ui_controls_tab_html(const int tab) {
    ui_windows_load();
    return tab >= 0 && tab < g_controls_tab_count && g_controls_tabs[tab].html ? g_controls_tabs[tab].html : "";
}

const char *ui_controls_tab_name(const int tab) {
    ui_windows_load();
    return tab >= 0 && tab < g_controls_tab_count ? g_controls_tabs[tab].name : "";
}

// ---------------------------------------------------------------- Krunker tables

// mapThumb(): the official map's public/img/maps card, else img/noimg.png (map_image "").
const char *ui_map_thumb(const char *name) {
    static char thumb[48];
    const int index = name ? map_catalog_find(name) : -1;
    const char *src = index >= 0 && !strcmp(map_catalog_at((size_t) index)->name, name)
                          ? map_catalog_thumb(map_catalog_at((size_t) index), thumb, sizeof(thumb)) : NULL;
    return src ? src : "img/noimg.png";
}

// ---------------------------------------------------------------- generators

// windows[0].genList() for the current view (basic/advanced) and tab: the captured tab
// with its `settingTab` row and the advanced switch matching the state. The settings
// rows' values are applied after rendering (ui_window_post -> ui_settings_apply_values).
static char *gen_settings(void) {
    const MenuUi *ui = menu_ui();
    const int view = ui->settings_view;
    const int tabs = ui_settings_view_tab_count(view);
    const int client_tab = ui_client_settings_tab(); // desktop client: "Client" after Krunker's tabs
    int tab = ui->settings_tab;
    if (tab != client_tab && (tab < 0 || tab >= tabs)) tab = 0;
    // the captured tab (or the first one for the Client tab: same header, settings replaced)
    const char *tpl = ui_settings_view_tab_html(view, tab == client_tab ? 0 : tab);
    StrBuf out = {0};
    const char *holder = strstr(tpl, "<div id=\"settHolder\">");
    const char *layout = strstr(tpl, "<div id=\"settingsTabLayout\">");
    if (!holder || !layout || layout > holder) {
        sb_append(&out, tpl);
        return sb_finish(&out);
    }
    // header up to the tab row: the advanced switch follows the view
    StrBuf head = {0};
    sb_append_n(&head, tpl, (size_t) (layout - tpl));
    char *head_s = sb_finish(&head);
    const char *chk = strstr(head_s, "id=\"typeBtn\"");
    if (chk) {
        const int checked = strstr(chk, " checked") && strstr(chk, " checked") < strchr(chk, '>');
        if (checked != (view == 1)) {
            const char *gt = strchr(chk, '>');
            sb_append_n(&out, head_s, (size_t) (chk - head_s));
            if (view == 1) {
                sb_append(&out, "id=\"typeBtn\" checked=\"\"");
                sb_append_n(&out, chk + strlen("id=\"typeBtn\""), (size_t) (gt - chk - strlen("id=\"typeBtn\"")));
            } else {
                // drop the checked attribute
                StrBuf attrs = {0};
                sb_append_n(&attrs, chk, (size_t) (gt - chk));
                char *a = sb_finish(&attrs);
                char *c = strstr(a, " checked=\"\"");
                if (c) memmove(c, c + strlen(" checked=\"\""), strlen(c + strlen(" checked=\"\"")) + 1);
                sb_append(&out, a);
                free(a);
            }
            sb_append(&out, gt);
            free(head_s);
            head_s = sb_finish(&out);
        }
    }
    sb_free(&out);
    sb_append(&out, head_s);
    free(head_s);
    // windows[0].getTabs(): one settingTab per tab, the active one tabANew
    sb_append(&out, "<div id=\"settingsTabLayout\">");
    for (int t = 0; t < tabs; t++) {
        if (t) sb_append(&out, "\n");
        sb_appendf(&out, "<div class=\"settingTab %s\" onmouseenter=\"playTick()\" onclick=\"playSelect(0.1);window.windows[0].changeTab(%d)\">",
                   t == tab ? "tabANew" : "", t);
        sb_append_esc(&out, ui_settings_view_tab_name(view, t));
        sb_append(&out, "</div>");
    }
    if (client_tab >= 0) // the desktop client's Client tab (ui_client_settings.c), as v7's own client adds it
        sb_appendf(&out, "\n<div class=\"settingTab %s\" onmouseenter=\"playTick()\" onclick=\"playSelect(0.1);window.windows[0].changeTab(%d)\">Client</div>",
                   tab == client_tab ? "tabANew" : "", client_tab);
    sb_append(&out, "</div></div>");
    sb_append(&out, "<div id=\"settHolder\">");
    if (tab == client_tab) {
        char *client = ui_client_settings_tab_html(0);
        sb_append(&out, client);
        free(client);
    } else {
        const char *body = holder + strlen("<div id=\"settHolder\">");
        size_t len = strlen(body);
        if (len >= 6 && !strcmp(body + len - 6, "</div>")) len -= 6; // the holder's own </div>
        StrBuf inner = {0};
        sb_append_n(&inner, body, len);
        char *inner_s = sb_finish(&inner);
        // acc && tabIndex == 0: the account's settings backup row comes from the capture of a
        // guest, so it is added here (social socket uploadS / getS)
        if (account_get() && tab == 0 && !strstr(inner_s, "uploadSettings()")) {
            sb_append(&out, "<div class='setBodH'><div class=\"settName\" style=\"margin-top:15px\">\n\t\t\t\tBackup Settings\n\t\t\t\t<div class=\"settingsBtn\" style=\"width: auto;\" onclick=\"uploadSettings()\">Upload</div>\n\t\t\t\t<div class=\"settingsBtn\" style=\"width: auto;\" onclick=\"fetchSettings()\">Download</div>\n\t\t\t</div></div>");
        }
        // the Sound tab gains the client's Audio Output row (ui_client_settings.c)
        char *ext = ui_client_settings_extend_tab(inner_s);
        sb_append(&out, ext ? ext : inner_s);
        free(ext);
        free(inner_s);
    }
    sb_append(&out, "</div>");
    return sb_finish(&out);
}

// The Custom Games (windows[9]), Host (windows[7]) and Servers (windows[1]) windows and the
// host window's read-back (ui_host_checked_*) live in servers_ui.c with the matchmaker client.

static char *gen_static(const char *key) {
    StrBuf out = {0};
    sb_append(&out, ui_windows_html(key));
    return sb_finish(&out);
}

// windows[] entries whose gen() KrunkNative writes; every other window v7 defines shows its
// captured markup (ui/windows/<id>.html: the guest state of the real page).
typedef struct {
    int id;
    char *(*gen)(void);
} WindowDef;

static char *gen_7(void) { return ui_controls_window_html(); } // live binds (ui_controls.c)
static char *gen_9(void) { // Select Spray: owned sprays signed in (ui_loadout.c), else the default sprays
    char *owned = ui_loadout_generate(9, NULL);
    return owned ? owned : gen_static("9");
}
static char *gen_23(void) { return hud_ui_player_list_html(); } // Player List (pListKey, hud_ui.c)
static char *gen_13(void) { return clan_window_gen(); }
static char *gen_14(void) { return store_window_gen(); } // Store (windows[13], store_ui.c)
static char *gen_25(void) { return download_window_gen(); } // Download Client: KrunkNative's packages (download_ui.c)
static char *gen_27(void) { return ranked_ui_window_gen(); } // Ranked (kept by KrunkNative, ranked_ui.c) // Clans (windows[12], clan_ui.c): guest LOGIN REQUIRED
// Change Loadout and the windows it opens, Changelog: ui_loadout.c
static char *gen_loadout(void) { return ui_loadout_gen(); }
#define LOADOUT_GEN(n) static char *gen_##n(void) { char *h = ui_loadout_generate(n, NULL); return h ? h : gen_static(#n); }
LOADOUT_GEN(15) LOADOUT_GEN(16) LOADOUT_GEN(17) LOADOUT_GEN(20) LOADOUT_GEN(26) LOADOUT_GEN(31)
LOADOUT_GEN(32) LOADOUT_GEN(34) LOADOUT_GEN(35) LOADOUT_GEN(38) LOADOUT_GEN(39)
LOADOUT_GEN(40) LOADOUT_GEN(41) LOADOUT_GEN(43) LOADOUT_GEN(44) LOADOUT_GEN(45) LOADOUT_GEN(46)
LOADOUT_GEN(50) LOADOUT_GEN(51) LOADOUT_GEN(52)
#undef LOADOUT_GEN

static char *gen_object_manager(void) { return object_manager_window_gen(OBJECT_MANAGER_WINDOW, NULL); }

static const WindowDef WINDOWS[] = {
    {SERVERS_WINDOW_CONNECT, servers_connect_window_gen}, // KrunkNative: the menu's Connect button (servers_ui.c)
    {OBJECT_MANAGER_WINDOW, gen_object_manager},          // KrunkNative: the Object Manager (object_manager_ui.c)
    {1, gen_settings},
    {3, gen_loadout},
    {7, gen_7},
    {9, gen_9},
    {13, gen_13},
    {14, gen_14},
    {25, gen_25},
    {27, gen_27},
    {15, gen_15},
    {16, gen_16},
    {17, gen_17},
    {20, gen_20},
    {23, gen_23},
    {26, gen_26},
    {31, gen_31},
    {32, gen_32},
    {34, gen_34},
    {35, gen_35},
    {38, gen_38},
    {39, gen_39},
    {40, gen_40},
    {41, gen_41},
    {43, gen_43},
    {44, gen_44},
    {45, gen_45},
    {46, gen_46},
    {50, gen_50},
    {51, gen_51},
    {52, gen_52},
};

static const WindowDef *find_window(const int id) {
    for (size_t i = 0; i < sizeof(WINDOWS) / sizeof(WINDOWS[0]); i++) {
        if (WINDOWS[i].id == id) return &WINDOWS[i];
    }
    return NULL;
}

// The window key ("<id>") when v7 has that window, NULL otherwise.
const char *ui_window_key(const int id) {
    static char key[8];
    if (id <= 0) return NULL;
    snprintf(key, sizeof(key), "%d", id);
    return ui_windows_meta(key) || find_window(id) ? key : NULL;
}

// #menuWindow class (windows[n].extraCls + "dark", stickyHeader) and inline style (width,
// height, maxHeight, borderRadius, overflow-y) of showWindow(id), as captured.
void ui_window_meta(const int id, const char **cls, const char **style) {
    char key[8];
    snprintf(key, sizeof(key), "%d", id);
    const UiWindowMeta *m = ui_windows_meta(key);
    if (cls) *cls = m ? m->cls : "dark";
    if (style) *style = m ? m->style : id == OBJECT_MANAGER_WINDOW ? "overflow-y: auto; width: 1120px;" : "overflow-y: auto;";
}

char *ui_window_generate(const int id, const char **header, int *sticky) {
    if (!ui_window_key(id)) return NULL;
    ui_windows_load();
    char key[8];
    snprintf(key, sizeof(key), "%d", id);
    const UiWindowMeta *m = ui_windows_meta(key);
    if (header) *header = m ? m->header : "";
    if (sticky) *sticky = m ? m->sticky : 0;
    // brain backend (backend_ui.h): the signed-in Account window and the live Servers
    // list replace the guest captures; NULL keeps them (no brain, guest)
    char *live = account_window_gen(id, header);
    if (!live) live = battlepass_window_gen(id, header); // v7.1.4 Battle Pass (battlepass_ui.c)
    if (!live) live = servers_window_gen(id, header);
    if (!live) live = object_manager_window_gen(id, header); // KrunkNative Object Manager (F1)
    if (live) return live;
    const WindowDef *w = find_window(id);
    return w ? w->gen() : gen_static(key);
}

void ui_img_url(const char *path, char *out, const size_t n) {
    if (!out || !n) return;
    while (path && *path == '/') path++;
    snprintf(out, n, "./%s", path ? path : "");
}

void ui_window_post(const int id) {
    // KrunkNative Object Manager: shown over the game without the menu around it (krunknative.css
    // html.kn-bare-window); menu_close_window takes the class off
    html_dom_set_class(html_dom_root(), "kn-bare-window", id == OBJECT_MANAGER_WINDOW);
    if (id == 1 || id == 32 || id == 41) { // 32 / 41: the reticle / scope pickers' settings rows
        ui_settings_apply_values(html_dom_by_id("menuWindow"));
    } else if (id == 5) {
        const MenuUi *ui = menu_ui();
        HtmlElem input = html_dom_by_id("accName");
        if (input) html_dom_set_value(input, ui->name_set ? ui->client->menu_name : "");
    }
    account_window_post(id); // brain: the saved krunker_username instead of the nickname
    battlepass_window_post(id); // windows[5].post(): the window width, scroll to the current level
}
