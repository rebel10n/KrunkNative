// The image collection popup that a URL setting's "collections" button opens (imagePopup(key)): pick a stored image
// for crosshairImage, customHitmarker, customGameOverlay and the other settings flagged `collection`
// (deob-10.0.7.js:218135; the flags at :211922, :212181, :212508, :214264-214348).
//
// Behaviour follows Krunker v10.0.7 (:214688-214775); the markup and wording are Box Office's own:
//   - five tabs; tab 0 also lists the built-in images (one, img/backgrounds/0.png, :202769);
//   - the player's own images are [name, url, tab] entries kept in storage "krk_custImgs" (:214691-214701);
//   - picking an image sets the setting to its URL and closes the popup (:214703-214709);
//   - an entry can be removed (:214710-214716);
//   - Add Image asks for a name (letters, digits, space, _ and - only), a URL and a tab (:214757-214775); the URL
//     must name a .jpg / .jpeg / .png / .gif / .webp / .ico image, not an SVG, and loses any quote marks (:214688).
#include <image_collection.h>

#include <cJSON.h>
#include <html_dom.h>
#include <i18n.h>
#include <ui_logic.h>
#include <v10_ui.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define IC_STORAGE_KEY "krk_custImgs"
#define IC_TABS 5
#define IC_MAX 200

typedef struct {
    char name[48];
    char url[512];
    int tab;
} IcImage;

static IcImage g_images[IC_MAX];
static int g_count, g_loaded;
static char g_key[48]; // the setting the open popup fills
static int g_tab;

static const char *tab_label(const int i) {
    switch (i) {
        case 0: return I18N("kn.imageCollection.tab0", "Backgrounds");
        case 1: return I18N("kn.imageCollection.tab1", "Overlays");
        case 2: return I18N("kn.imageCollection.tab2", "Scopes");
        case 3: return I18N("kn.imageCollection.tab3", "Crosshairs");
        default: return I18N("kn.imageCollection.tab4", "Icons");
    }
}

static int clamp_tab(const int t) { return t < 0 ? 0 : t >= IC_TABS ? IC_TABS - 1 : t; }

// Letters, digits, space, _ and - only (:214763).
void image_collection_clean_name(char *out, const size_t size, const char *in) {
    size_t o = 0;
    for (const char *p = in ? in : ""; *p && o + 1 < size; p++) {
        const unsigned char c = (unsigned char) *p;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == ' ' || c == '_' || c == '-') out[o++] = (char) c;
    }
    out[o] = 0;
}

// The URL an image may use, or 0 (:214688): an image extension somewhere in it, no SVG, an http(s) URL, quotes dropped.
int image_collection_clean_url(char *out, const size_t size, const char *in) {
    size_t o = 0;
    for (const char *p = in ? in : ""; *p && o + 1 < size; p++)
        if (*p != '\'' && *p != '"' && *p != '<' && *p != '>' && (unsigned char) *p > ' ') out[o++] = *p;
    out[o] = 0;
    if (strncasecmp(out, "https://", 8) && strncasecmp(out, "http://", 7)) return 0;
    char lower[512];
    size_t i = 0;
    for (; out[i] && i + 1 < sizeof(lower); i++) lower[i] = (char) (out[i] >= 'A' && out[i] <= 'Z' ? out[i] + 32 : out[i]);
    lower[i] = 0;
    if (strstr(lower, ".svg")) return 0;
    static const char *const ext[] = {".jpg", ".jpeg", ".png", ".gif", ".webp", ".ico"};
    for (size_t e = 0; e < sizeof(ext) / sizeof(ext[0]); e++)
        if (strstr(lower, ext[e])) return 1;
    return 0;
}

static void load(void) {
    if (g_loaded) return;
    g_loaded = 1;
    g_count = 0;
    char buf[32768];
    if (html_storage_get(IC_STORAGE_KEY, buf, sizeof(buf)) < 0) return;
    cJSON *arr = cJSON_Parse(buf);
    const cJSON *row;
    cJSON_ArrayForEach(row, arr) {
        if (g_count >= IC_MAX || !cJSON_IsArray(row)) continue;
        const char *name = cJSON_GetStringValue(cJSON_GetArrayItem(row, 0));
        const char *url = cJSON_GetStringValue(cJSON_GetArrayItem(row, 1));
        IcImage *im = &g_images[g_count];
        image_collection_clean_name(im->name, sizeof(im->name), name);
        if (!im->name[0] || !image_collection_clean_url(im->url, sizeof(im->url), url)) continue;
        im->tab = clamp_tab((int) cJSON_GetNumberValue(cJSON_GetArrayItem(row, 2)));
        g_count++;
    }
    cJSON_Delete(arr);
}

static void save(void) {
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < g_count; i++) {
        cJSON *row = cJSON_CreateArray();
        cJSON_AddItemToArray(row, cJSON_CreateString(g_images[i].name));
        cJSON_AddItemToArray(row, cJSON_CreateString(g_images[i].url));
        cJSON_AddItemToArray(row, cJSON_CreateNumber(g_images[i].tab));
        cJSON_AddItemToArray(arr, row);
    }
    char *text = cJSON_PrintUnformatted(arr);
    if (text) html_storage_set(IC_STORAGE_KEY, text);
    free(text);
    cJSON_Delete(arr);
}

static void esc(char *out, const size_t size, const char *in) {
    size_t o = 0;
    for (const char *p = in ? in : ""; *p && o + 7 < size; p++) {
        const char *r = *p == '&' ? "&amp;" : *p == '<' ? "&lt;" : *p == '>' ? "&gt;" : *p == '"' ? "&quot;" : *p == '\'' ? "&#39;" : NULL;
        if (r) {
            memcpy(out + o, r, strlen(r));
            o += strlen(r);
        } else {
            out[o++] = *p;
        }
    }
    out[o] = 0;
}

static void sb_add(char **buf, size_t *len, size_t *cap, const char *s) {
    const size_t n = strlen(s);
    if (*len + n + 1 > *cap) {
        size_t c = *cap ? *cap * 2 : 4096;
        while (c < *len + n + 1) c *= 2;
        char *nb = realloc(*buf, c);
        if (!nb) return;
        *buf = nb;
        *cap = c;
    }
    memcpy(*buf + *len, s, n + 1);
    *len += n;
}

static void tile(char **b, size_t *l, size_t *c, const char *name, const char *url, const char *pick, const int removable, const int index) {
    char ename[160], eurl[1100], line[2048];
    esc(ename, sizeof(ename), name);
    esc(eurl, sizeof(eurl), url);
    snprintf(line, sizeof(line),
             "<div class='imageListItem' title='%s' style='background-image:url(\"%s\")'>"
             "<div class='imageActionOvrl'><div class='imageActionHol'>"
             "<div class='imageActionB' onclick='%s' title='%s'><span class='material-icons'>check</span></div>",
             ename, eurl, pick, I18N("kn.imageCollection.use", "Use this image"));
    sb_add(b, l, c, line);
    if (removable) {
        snprintf(line, sizeof(line),
                 "<div class='imageActionB' onclick='removeImage(%d)' title='%s'><span class='material-icons'>delete</span></div>",
                 index, I18N("kn.imageCollection.remove", "Remove from my images"));
        sb_add(b, l, c, line);
    }
    snprintf(line, sizeof(line), "</div></div><div class='kn-image-name'>%s</div></div>", ename);
    sb_add(b, l, c, line);
}

static void render(void) {
    char *b = NULL;
    size_t l = 0, c = 0;
    char line[1024];
    snprintf(line, sizeof(line), "<div class='imageListHeader'>%s<div class='imageListAdd' onclick='imageNewPopup()'>%s</div></div>",
             I18N("kn.imageCollection.title", "My Images"), I18N("kn.imageCollection.add", "Add an image"));
    sb_add(&b, &l, &c, line);
    sb_add(&b, &l, &c, "<div id='imagesTabLayout'>");
    for (int t = 0; t < IC_TABS; t++) {
        snprintf(line, sizeof(line), "<div class='imageTab%s' onclick='imagePopup(\"\",%d)'>%s</div>", t == g_tab ? " tabAIMG" : "", t, tab_label(t));
        sb_add(&b, &l, &c, line);
    }
    sb_add(&b, &l, &c, "</div><div class='kn-image-grid'>");
    int shown = 0;
    for (int i = 0; i < g_count; i++) {
        if (g_images[i].tab != g_tab) continue;
        char pick[48];
        snprintf(pick, sizeof(pick), "selectImage(-2,%d)", i);
        tile(&b, &l, &c, g_images[i].name, g_images[i].url, pick, 1, i);
        shown++;
    }
    if (g_tab == 0) { // the built-in background (:214737-214744)
        tile(&b, &l, &c, I18N("kn.imageCollection.builtin0", "Night Snowfall"), "img/backgrounds/0.png", "selectImage(0,-1)", 0, 0);
        shown++;
    }
    if (!shown) {
        snprintf(line, sizeof(line), "<div class='kn-image-empty'>%s</div>", I18N("kn.imageCollection.empty", "No images here yet."));
        sb_add(&b, &l, &c, line);
    }
    sb_add(&b, &l, &c, "</div>");
    v10_popup_show("imagePop", b ? b : "", render);
    free(b);
}

static void render_new(void) {
    char *b = NULL;
    size_t l = 0, c = 0;
    char line[1024];
    snprintf(line, sizeof(line), "<div class='imageListHeader'>%s</div>", I18N("kn.imageCollection.newTitle", "Add an image"));
    sb_add(&b, &l, &c, line);
    snprintf(line, sizeof(line), "<div class='kn-image-field'>%s<input type='text' id='newCustName' class='inputGrey2' maxlength='40'></div>",
             I18N("kn.imageCollection.name", "Name"));
    sb_add(&b, &l, &c, line);
    snprintf(line, sizeof(line), "<div class='kn-image-field'>%s<select id='newCustCat' class='inputGrey2'>", I18N("kn.imageCollection.tab", "Tab"));
    sb_add(&b, &l, &c, line);
    for (int t = 0; t < IC_TABS; t++) {
        snprintf(line, sizeof(line), "<option value='%d'%s>%s</option>", t, t == g_tab ? " selected" : "", tab_label(t));
        sb_add(&b, &l, &c, line);
    }
    sb_add(&b, &l, &c, "</select></div>");
    snprintf(line, sizeof(line), "<div class='kn-image-field'>%s<input type='url' id='newCustImg' class='inputGrey2' placeholder='https://'></div>",
             I18N("kn.imageCollection.url", "Image link (PNG, JPG, GIF, WebP or ICO)"));
    sb_add(&b, &l, &c, line);
    snprintf(line, sizeof(line), "<div class='kn-image-actions'><div class='button buttonG' onclick='addNewImage()'>%s</div>"
                                 "<div class='button buttonR' onclick='imagePopup(\"\",%d)'>%s</div></div>",
             I18N("kn.imageCollection.save", "Save"), g_tab, I18N("kn.imageCollection.back", "Back"));
    sb_add(&b, &l, &c, line);
    v10_popup_show("imageNewPop", b ? b : "", render_new);
    free(b);
}

// Adds an image (the Add form's values); 0 and a message on a bad name or URL.
int image_collection_add(const char *name, const char *url, const int tab, const char **why) {
    load();
    if (g_count >= IC_MAX) {
        if (why) *why = I18N("kn.imageCollection.full", "Your image list is full");
        return 0;
    }
    IcImage im = {0};
    image_collection_clean_name(im.name, sizeof(im.name), name);
    if (!im.name[0]) {
        if (why) *why = I18N("kn.imageCollection.badName", "That name can't be used");
        return 0;
    }
    if (!image_collection_clean_url(im.url, sizeof(im.url), url)) {
        if (why) *why = I18N("kn.imageCollection.badUrl", "That link isn't a supported image");
        return 0;
    }
    im.tab = clamp_tab(tab);
    g_images[g_count++] = im;
    save();
    return 1;
}

int image_collection_remove(const int index) {
    load();
    if (index < 0 || index >= g_count) return 0;
    memmove(&g_images[index], &g_images[index + 1], sizeof(g_images[0]) * (size_t) (g_count - index - 1));
    g_count--;
    save();
    return 1;
}

int image_collection_count(void) {
    load();
    return g_count;
}

const char *image_collection_url(const int index) {
    load();
    return index >= 0 && index < g_count ? g_images[index].url : NULL;
}

void image_collection_reload(void) { g_loaded = 0; }

static void set_url_setting(const char *url) {
    if (!g_key[0] || !url) return;
    ui_settings_set_string(g_key, url);
}

int image_collection_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    if (!name) return -1;
    if (!strcmp(name, "imagePopup")) {
        load();
        const char *key = argc > 0 && args[0].kind == HTML_ARG_STR ? args[0].str : "";
        if (key && key[0]) snprintf(g_key, sizeof(g_key), "%s", key);
        g_tab = clamp_tab(argc > 1 && args[1].kind == HTML_ARG_NUM ? (int) args[1].num : key && key[0] ? 0 : g_tab);
        render();
        return 0;
    }
    if (!strcmp(name, "imageNewPopup")) {
        render_new();
        return 0;
    }
    if (!strcmp(name, "addNewImage")) {
        char nm[64] = "", url[600] = "", cat[8] = "0";
        HtmlElem e;
        if ((e = html_dom_by_id("newCustName"))) html_dom_get_value(e, nm, sizeof(nm));
        if ((e = html_dom_by_id("newCustImg"))) html_dom_get_value(e, url, sizeof(url));
        if ((e = html_dom_by_id("newCustCat"))) html_dom_get_value(e, cat, sizeof(cat));
        const char *why = NULL;
        const int tab = atoi(cat);
        if (!image_collection_add(nm, url, tab, &why)) {
            ui_notify(why ? why : "");
            return 0;
        }
        g_tab = clamp_tab(tab);
        render();
        return 0;
    }
    if (!strcmp(name, "removeImage")) {
        if (argc > 0 && args[0].kind == HTML_ARG_NUM) image_collection_remove((int) args[0].num);
        render();
        return 0;
    }
    if (!strcmp(name, "selectImage")) {
        const int builtin = argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : -2;
        const int index = argc > 1 && args[1].kind == HTML_ARG_NUM ? (int) args[1].num : -1;
        if (builtin >= 0) set_url_setting("img/backgrounds/0.png");
        else set_url_setting(image_collection_url(index));
        v10_popup_close();
        return 0;
    }
    return -1;
}
