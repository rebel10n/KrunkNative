// image_collection_test: a URL setting's image collection (client/src/ui/image_collection.c) against stubbed storage,
// DOM, popup and settings.
#include <image_collection.h>

#include <i18n.h>
#include <ui_logic.h>
#include <v10_ui.h>

#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); g_fail = 1; } } while (0)

static char g_store[32768];
static int g_has_store;
int html_storage_get(const char *key, char *buf, size_t n) {
    (void) key;
    if (!g_has_store) return -1;
    snprintf(buf, n, "%s", g_store);
    return (int) strlen(buf);
}
void html_storage_set(const char *key, const char *value) {
    (void) key;
    snprintf(g_store, sizeof(g_store), "%s", value);
    g_has_store = 1;
}
const char *i18n_tr(const char *key, const char *english) { (void) key; return english; }

static char g_popup[16384], g_popup_cls[32], g_notice[256], g_set_key[64], g_set_val[600];
static int g_closed;
void v10_popup_show(const char *cls, const char *html, V10RenderFn render) {
    (void) render;
    snprintf(g_popup_cls, sizeof(g_popup_cls), "%s", cls);
    snprintf(g_popup, sizeof(g_popup), "%s", html);
}
void v10_popup_close(void) { g_closed = 1; }
void ui_notify(const char *html) { snprintf(g_notice, sizeof(g_notice), "%s", html); }
void ui_settings_set_string(const char *key, const char *value) {
    snprintf(g_set_key, sizeof(g_set_key), "%s", key);
    snprintf(g_set_val, sizeof(g_set_val), "%s", value);
}

// the Add form's three inputs
static const char *g_in_name = "", *g_in_url = "", *g_in_cat = "0";
HtmlElem html_dom_by_id(const char *id) {
    if (!strcmp(id, "newCustName")) return 1;
    if (!strcmp(id, "newCustImg")) return 2;
    if (!strcmp(id, "newCustCat")) return 3;
    return 0;
}
int html_dom_get_value(HtmlElem e, char *buf, size_t n) {
    snprintf(buf, n, "%s", e == 1 ? g_in_name : e == 2 ? g_in_url : g_in_cat);
    return (int) strlen(buf);
}

static HtmlArg str(const char *s) { return (HtmlArg) {HTML_ARG_STR, 0, s}; }
static HtmlArg num(double v) { return (HtmlArg) {HTML_ARG_NUM, v, NULL}; }

int main(void) {
    char out[600];
    // names (deob-10.0.7.js:214763) and links (:214688)
    image_collection_clean_name(out, sizeof(out), "My <b>Scope</b>_1-x!");
    CHECK(!strcmp(out, "My bScopeb_1-x"), "name keeps letters, digits, space, _ and -");
    CHECK(image_collection_clean_url(out, sizeof(out), "https://example.com/a.png"), "png accepted");
    CHECK(image_collection_clean_url(out, sizeof(out), "https://example.com/A.WEBP?x=1"), "webp in any case");
    CHECK(!image_collection_clean_url(out, sizeof(out), "https://example.com/a.svg"), "svg refused");
    CHECK(!image_collection_clean_url(out, sizeof(out), "https://example.com/a.svg#.png"), "svg refused even with a png part");
    CHECK(!image_collection_clean_url(out, sizeof(out), "https://example.com/page.html"), "not an image refused");
    CHECK(!image_collection_clean_url(out, sizeof(out), "javascript:alert(1)//.png"), "only http(s) links");
    CHECK(image_collection_clean_url(out, sizeof(out), "https://e.com/\"a'.png") && !strchr(out, '"') && !strchr(out, '\''), "quotes dropped");

    // open the collection for a setting: tab 0 shows the built-in image, no own images yet
    HtmlArg open[1] = {str("customGameOverlay")};
    image_collection_handle("imagePopup", NULL, open, 1);
    CHECK(!strcmp(g_popup_cls, "imagePop") && strstr(g_popup, "img/backgrounds/0.png"), "built-in image on tab 0");
    CHECK(strstr(g_popup, "tabAIMG") != NULL, "a tab is active");

    // add through the form, onto tab 2
    g_in_name = "Red dot";
    g_in_url = "https://example.com/dot.png";
    g_in_cat = "2";
    image_collection_handle("addNewImage", NULL, NULL, 0);
    CHECK(image_collection_count() == 1 && strstr(g_store, "dot.png") && strstr(g_store, "Red dot"), "added and stored");
    CHECK(strstr(g_popup, "dot.png") != NULL && !strstr(g_popup, "img/backgrounds/0.png"), "tab 2 shows the new image, not the built-in");

    // a bad link is refused with a message and nothing is stored
    g_in_name = "Bad";
    g_in_url = "https://example.com/x.svg";
    image_collection_handle("addNewImage", NULL, NULL, 0);
    CHECK(image_collection_count() == 1 && strstr(g_notice, "supported image"), "svg add refused");

    // picking sets the setting to the image's link and closes the popup
    HtmlArg pick[2] = {num(-2), num(0)};
    image_collection_handle("selectImage", NULL, pick, 2);
    CHECK(!strcmp(g_set_key, "customGameOverlay") && !strcmp(g_set_val, "https://example.com/dot.png") && g_closed, "pick sets the setting");

    // the built-in pick
    g_closed = 0;
    HtmlArg pick0[2] = {num(0), num(-1)};
    image_collection_handle("selectImage", NULL, pick0, 2);
    CHECK(!strcmp(g_set_val, "img/backgrounds/0.png") && g_closed, "built-in pick");

    // the list survives a reload from storage, and a removed entry is gone
    image_collection_reload();
    CHECK(image_collection_count() == 1, "reloaded from storage");
    HtmlArg rm[1] = {num(0)};
    image_collection_handle("removeImage", NULL, rm, 1);
    image_collection_reload();
    CHECK(image_collection_count() == 0, "removed and saved");

    // a hand-edited store with a bad row keeps only the good rows
    html_storage_set("krk_custImgs", "[[\"ok\",\"https://e.com/a.jpg\",9],[\"\",\"https://e.com/b.jpg\",0],[\"x\",\"ftp://e.com/c.png\",0],5]");
    image_collection_reload();
    CHECK(image_collection_count() == 1, "bad stored rows skipped");

    CHECK(image_collection_handle("openSocial", NULL, NULL, 0) == -1, "other names are not handled");
    if (!g_fail) printf("image_collection_test: OK\n");
    return g_fail;
}
