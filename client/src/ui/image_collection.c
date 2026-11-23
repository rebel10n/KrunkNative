// The image collection popup that a URL setting's "collections" button opens (imagePopup(key)): pick a stored image
// for crosshairImage, customHitmarker, customGameOverlay and the other settings flagged `collection`
// (deob-10.0.7.js:218135; the flags at :211922, :212181, :212508, :214264-214348), and the hub's Background Image
// (hub-10.0.7.js:6335-6440, the same code and markup).
//
// Krunker v10.0.7 (deob-10.0.7.js:214688-214775), markup and wording included:
//   - five tabs (Backgrounds, Overlays, Scopes, Reticles, Icons); tab 0 also lists the built-in images (one,
//     "Midnight Snow", /img/backgrounds/0.png, :202769-202772);
//   - the player's own images are [name, url, tab] entries kept in storage "krk_custImgs" (:214691-214701);
//   - Select sets the setting to the image's URL and closes the popup (:214703-214709; the hub opens its settings
//     again, hub-10.0.7.js:6359-6368);
//   - Delete removes the entry and shows tab 0 (:214710-214716);
//   - Add Image asks for a name (letters, digits, space, _ and - only), a category and a URL (:214750-214775); the URL
//     must name a .jpg / .jpeg / .png / .gif / .webp / .ico image, lose its quote marks, pass isURL and not be an SVG
//     (:214688). A failure is the response popup's "Something Went Wrong" / "Invalid Name" / "Invalid Image".
#include <image_collection.h>

#include <cJSON.h>
#include <html_dom.h>
#include <i18n.h>
#include <ui_logic.h>
#include <v10_ui.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define IC_STORAGE_KEY "krk_custImgs"
#define IC_TABS 5
#define IC_MAX 200
#define IC_STORE_MAX (IC_MAX * 2400)

typedef struct {
    char name[128];  // [0] as stored
    char url[2048];  // [1] as stored
    int tab;         // [2]
} IcImage;

static IcImage g_images[IC_MAX];
static int g_count, g_loaded;
static char g_key[48]; // the setting the open popup fills
static int g_tab;
static const ImageCollectionHost *g_host; // NULL: the menu page

void image_collection_set_host(const ImageCollectionHost *host) { g_host = host; }

static void popup_show(const char *cls, const char *html, void (*rerender)(void)) {
    if (g_host && g_host->show) g_host->show(cls, html);
    else v10_popup_show(cls, html, rerender);
}

static const char *const k_tabs[IC_TABS] = {"Backgrounds", "Overlays", "Scopes", "Reticles", "Icons"};

static int clamp_tab(const int t) { return t < 0 ? 0 : t >= IC_TABS ? IC_TABS - 1 : t; }

// Letters, digits, space, _ and - (/[^a-zA-Z0-9 _-]+/gim removed, :214763).
void image_collection_clean_name(char *out, const size_t size, const char *in) {
    size_t o = 0;
    for (const char *p = in ? in : ""; *p && o + 1 < size; p++) {
        const unsigned char c = (unsigned char) *p;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == ' ' || c == '_' || c == '-') out[o++] = (char) c;
    }
    out[o] = 0;
}

static void url_run(const char **p, const char *set) {
    while (**p && (isalnum((unsigned char) **p) || strchr(set, **p))) (*p)++;
}

// isURL (config-10.0.7.js:4102-4108), run by hand:
// /^(https?:\/\/)?((([a-z\d]([a-z\d-]*[a-z\d])*)\.)+[a-z]{2,}|((\d{1,3}\.){3}\d{1,3}))(\:\d+)?(\/[-a-z\d%@_.~+&:]*)*
//  (\?[;&a-z\d%@_.,~+&:=-]*)?(\#[-a-z\d_]*)?$/i
int image_collection_is_url(const char *u) {
    if (!u || !*u) return 0;
    const char *p = u;
    if (!strncasecmp(p, "https://", 8)) p += 8;
    else if (!strncasecmp(p, "http://", 7)) p += 7;
    const char *h = p;
    while (*p && (isalnum((unsigned char) *p) || *p == '.' || *p == '-')) p++;
    const size_t hn = (size_t) (p - h);
    if (!hn) return 0;
    int ok;
    { // (\d{1,3}\.){3}\d{1,3}
        int groups = 0, digits = 0, bad = 0;
        for (size_t i = 0; i < hn && !bad; i++) {
            if (isdigit((unsigned char) h[i])) bad |= ++digits > 3;
            else if (h[i] == '.') {
                bad |= !digits;
                digits = 0;
                groups++;
            } else bad = 1;
        }
        ok = !bad && groups == 3 && digits;
    }
    if (!ok) { // (label\.)+[a-z]{2,}, each label alphanumeric at both ends
        const char *dot = NULL;
        for (size_t i = 0; i < hn; i++)
            if (h[i] == '.') dot = h + i;
        if (dot) {
            ok = p - (dot + 1) >= 2;
            for (const char *t = dot + 1; t < p; t++) ok &= isalpha((unsigned char) *t) != 0;
            for (const char *l = h; ok && l < dot;) {
                const char *e = memchr(l, '.', (size_t) (dot - l) + 1);
                ok &= e > l && isalnum((unsigned char) l[0]) && isalnum((unsigned char) e[-1]);
                l = e + 1;
            }
        }
    }
    if (!ok) return 0;
    if (*p == ':') {
        const char *d = ++p;
        while (isdigit((unsigned char) *p)) p++;
        if (p == d) return 0;
    }
    while (*p == '/') {
        p++;
        url_run(&p, "-%@_.~+&:");
    }
    if (*p == '?') {
        p++;
        url_run(&p, ";&%@_.,~+:=-");
    }
    if (*p == '#') {
        p++;
        url_run(&p, "-_");
    }
    return !*p;
}

// iîîìiíì(url) (:214688): /\.(jpg|jpeg|png|gif|webp|ico)/ somewhere in it, the quote marks dropped, then isURL and no
// ".svg"; 0 = null.
int image_collection_clean_url(char *out, const size_t size, const char *in) {
    out[0] = 0;
    static const char *const ext[] = {".jpg", ".jpeg", ".png", ".gif", ".webp", ".ico"};
    int has = 0;
    for (size_t e = 0; in && e < sizeof(ext) / sizeof(ext[0]); e++) has |= strstr(in, ext[e]) != NULL;
    if (!has) return 0;
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < size; p++)
        if (*p != '\'' && *p != '"') out[o++] = *p;
    out[o] = 0;
    if (!image_collection_is_url(out) || strstr(out, ".svg")) {
        out[0] = 0;
        return 0;
    }
    return 1;
}

// The stored list as setCustomImages keeps it: every [name, url, tab] row, its index the Delete button's
static void load(void) {
    if (g_loaded) return;
    g_loaded = 1;
    g_count = 0;
    char *buf = malloc(IC_STORE_MAX);
    if (!buf) return;
    if (html_storage_get(IC_STORAGE_KEY, buf, IC_STORE_MAX) < 0) {
        free(buf);
        return;
    }
    cJSON *arr = cJSON_Parse(buf);
    free(buf);
    const cJSON *row;
    cJSON_ArrayForEach(row, arr) {
        // a row that is not [string, ...] stops v10's imagePopup (iïíííìï[0].replace); it is left out here
        const char *name = cJSON_GetStringValue(cJSON_GetArrayItem(row, 0));
        if (g_count >= IC_MAX || !cJSON_IsArray(row) || !name) continue;
        const char *url = cJSON_GetStringValue(cJSON_GetArrayItem(row, 1));
        const cJSON *tab = cJSON_GetArrayItem(row, 2);
        IcImage *im = &g_images[g_count++];
        snprintf(im->name, sizeof(im->name), "%s", name);
        snprintf(im->url, sizeof(im->url), "%s", url ? url : "");
        // iiïíîìì(parseInt(entry[2]) || 0, 0, 4)
        im->tab = clamp_tab(cJSON_IsNumber(tab) ? (int) tab->valuedouble : cJSON_IsString(tab) ? atoi(tab->valuestring) : 0);
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

// The setting's key as the markup carries it (&quot;key&quot;): the key has no quotes or markup of its own
static void key_attr(char *out, const size_t n) {
    size_t o = 0;
    for (const char *p = g_key; *p && o + 1 < n; p++)
        if (*p != '"' && *p != '\'' && *p != '<' && *p != '>' && *p != '&') out[o++] = *p;
    out[o] = 0;
}

// imagePopup(key, tab) (:214718-214749)
static void render(void) {
    char *b = NULL;
    size_t l = 0, c = 0;
    char key[64], line[6144];
    key_attr(key, sizeof(key));
    snprintf(line, sizeof(line),
             "<div class='imageListHeader'>Image Collection<div class='imageListAdd' onclick='window.imageNewPopup(&quot;%s&quot;)' onmouseenter='playTick()'>Add Image</div></div>",
             key);
    sb_add(&b, &l, &c, line);
    sb_add(&b, &l, &c, "<div id='skinList' style='margin-top:18px'>");
    sb_add(&b, &l, &c, "<div id='imagesTabLayout'>");
    for (int t = 0; t < IC_TABS; t++) {
        snprintf(line, sizeof(line), "%s<div class='imageTab %s' onmouseenter='playTick()'\n\t\tonclick='playSelect(0.1);imagePopup(&quot;%s&quot;, %d)'>%s</div>",
                 t ? "\n" : "", t == g_tab ? "tabAIMG" : "", key, t, k_tabs[t]);
        sb_add(&b, &l, &c, line);
    }
    sb_add(&b, &l, &c, "</div>");
    for (int i = 0; i < g_count; i++) {
        if (g_images[i].tab != g_tab) continue;
        char name[128], url[2048];
        image_collection_clean_name(name, sizeof(name), g_images[i].name);
        const int ok = image_collection_clean_url(url, sizeof(url), g_images[i].url);
        snprintf(line, sizeof(line),
                 "<div class='imageListItem'><div class='imageActionOvrl'><div class='imageActionHol'><div class='imageActionB' onclick='selectImage(-2, &quot;%s&quot;, &quot;%s&quot;)' title='Select' style='margin-top:0px;'><span class='material-icons' style='font-size:50px;color:#fff;'>play_arrow</span></div><div class='imageActionSep' style='margin-left:2px'></div><div class='imageActionB' onclick='removeImage(%d, &quot;%s&quot;)' title='Delete' style='margin-top:0px;'><span class='material-icons' style='font-size:50px;color:#fff;'>delete_forever</span></div></div></div><div class='imageListThumbH'><img loading='lazy' class='imageListThumb' draggable='false' src='%s' onmouseenter='playTick()' /></div><div class='imageListName'>%s<span class='imageListType'>Custom</span></div></div>",
                 ok ? url : "null", key, i, key, ok ? url : "", name[0] ? name : "Custom");
        sb_add(&b, &l, &c, line);
    }
    if (g_tab == 0) { // the built-in images (:214737-214745)
        snprintf(line, sizeof(line),
                 "<div class='imageListItem'><div class='imageActionOvrl'><div class='imageActionHol'><div class='imageActionB' onclick='selectImage(0, null, &quot;%s&quot;)' title='Select' style='margin-top:0px;'><span class='material-icons' style='font-size:50px;color:#fff;'>play_arrow</span></div></div></div><div class='imageListThumbH'><img loading='lazy' class='imageListThumb' draggable='false' src='img/backgrounds/0.png' onmouseenter='playTick()' /></div><div class='imageListName'>Midnight Snow<span class='imageListType'>Default</span></div></div>",
                 key);
        sb_add(&b, &l, &c, line);
    }
    sb_add(&b, &l, &c, "</div>");
    popup_show("imagePop", b ? b : "", render);
    free(b);
}

// imageNewPopup(key) (:214750-214756)
static void render_new(void) {
    char *b = NULL;
    size_t l = 0, c = 0;
    char key[64], line[1024];
    key_attr(key, sizeof(key));
    sb_add(&b, &l, &c,
           "<div class='imageListHeader' style='font-size: 26px;'>New Image</div><div style='font-size:20px;color:#fff'><div style='margin-top: 3px;font-size: 22px;color: rgba(255, 255, 255, 0.7);'>Name:<input type='text' id='newCustName' class='inputGrey2' placeholder='Name'></div><div style='margin-top: 3px;font-size: 22px;color: rgba(255, 255, 255, 0.7);'>Category:<select id='newCustCat' class='inputGrey2'>");
    for (int t = 0; t < IC_TABS; t++) {
        snprintf(line, sizeof(line), "%s<option value='%d'>%s</option>", t ? "\n" : "", t, k_tabs[t]);
        sb_add(&b, &l, &c, line);
    }
    snprintf(line, sizeof(line),
             "</select></div><div style='margin-top: 4px;font-size: 22px;color: rgba(255, 255, 255, 0.7);'>Image:<input type='url' id='newCustImg' class='inputGrey2' placeholder='Image URL'></div><div class='mapFndB' onclick='window.addNewImage(&quot;%s&quot;)' onmouseover='playTick(0.1)' style='background-color:#fc5a4e;width: 380px;margin-top: 10px;'>Add</div></div>",
             key);
    sb_add(&b, &l, &c, line);
    popup_show("imageNewPop", b ? b : "", render_new);
    free(b);
}

// iíïííiì(null, text) (:222910-222916): the response popup's error
static void error_popup(const char *text) {
    char html[512];
    snprintf(html, sizeof(html),
             "<div style='width:100%%;text-align:center'><div><i class='material-icons' style='font-size:80px;color:#e33b3b'>clear</i></div><div style='font-size:20px;color:rgba(255,255,255,0.5)'>%s</div></div>",
             text);
    popup_show("responsePop", html, NULL);
}

// addNewImage's checks and push (:214757-214775); 0 and the popup's text on a failure.
int image_collection_add(const char *name, const char *url, const int tab, const char **why) {
    load();
    if (!name || !*name || !url || !*url) {
        if (why) *why = "Something Went Wrong";
        return 0;
    }
    if (g_count >= IC_MAX) { // this host's store is bounded
        if (why) *why = "Something Went Wrong";
        return 0;
    }
    IcImage im = {0};
    image_collection_clean_name(im.name, sizeof(im.name), name);
    if (!im.name[0]) {
        if (why) *why = "Invalid Name";
        return 0;
    }
    if (!image_collection_clean_url(im.url, sizeof(im.url), url)) {
        if (why) *why = "Invalid Image";
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
    if (g_host && g_host->set) g_host->set(g_key, url);
    else {
        ui_settings_set_string(g_key, url);
        ui_settings_user_changed(g_key);
    }
}

static const char *arg_str(const HtmlArg *args, const int argc, const int i) {
    return i < argc && args[i].kind == HTML_ARG_STR && args[i].str ? args[i].str : NULL;
}

static void take_key(const HtmlArg *args, const int argc, const int i) {
    const char *key = arg_str(args, argc, i);
    if (key && key[0] && strcmp(key, "undefined") && strcmp(key, "null")) snprintf(g_key, sizeof(g_key), "%s", key);
}

static int is(const char *name, const char *fn) {
    return !strcmp(name, fn) || (!strncmp(name, "window.", 7) && !strcmp(name + 7, fn));
}

int image_collection_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    if (!name) return -1;
    if (is(name, "imagePopup")) { // imagePopup(key, tab): parseInt(tab) || 0
        load();
        take_key(args, argc, 0);
        const char *ts = arg_str(args, argc, 1);
        g_tab = clamp_tab(argc > 1 && args[1].kind == HTML_ARG_NUM ? (int) args[1].num : ts ? atoi(ts) : 0);
        render();
        return 0;
    }
    if (is(name, "imageNewPopup")) {
        take_key(args, argc, 0);
        render_new();
        return 0;
    }
    if (is(name, "addNewImage")) {
        take_key(args, argc, 0);
        char nm[256] = "", url[2048] = "", cat[8] = "0";
        HtmlElem e;
        if ((e = html_dom_by_id("newCustName"))) html_dom_get_value(e, nm, sizeof(nm));
        if ((e = html_dom_by_id("newCustImg"))) html_dom_get_value(e, url, sizeof(url));
        if ((e = html_dom_by_id("newCustCat"))) html_dom_get_value(e, cat, sizeof(cat));
        const char *why = NULL;
        if (!image_collection_add(nm, url, atoi(cat), &why)) {
            error_popup(why ? why : "Something Went Wrong");
            return 0;
        }
        g_tab = 0; // imagePopup(key)
        render();
        return 0;
    }
    if (is(name, "removeImage")) { // removeImage(index, key)
        take_key(args, argc, 1);
        if (argc > 0 && args[0].kind == HTML_ARG_NUM && image_collection_remove((int) args[0].num)) {
            g_tab = 0;
            render();
        }
        return 0;
    }
    if (is(name, "selectImage")) { // selectImage(index, url, key): index >= 0 a built-in image
        take_key(args, argc, 2);
        const int builtin = argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : -2;
        char url[2048] = "";
        // the markup's own URL passes iîîìiíì again: "null" (an entry that failed it) sets the setting's default
        if (builtin >= 0) snprintf(url, sizeof(url), "img/backgrounds/0.png");
        else image_collection_clean_url(url, sizeof(url), arg_str(args, argc, 1));
        set_url_setting(url);
        if (g_host && g_host->close) g_host->close();
        else v10_popup_close();
        return 0;
    }
    return -1;
}
