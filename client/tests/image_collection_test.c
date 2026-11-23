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

static char g_popup[32768], g_popup_cls[32], g_notice[256], g_set_key[64], g_set_val[600];
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
void ui_settings_user_changed(const char *key) { (void) key; } // the preset becomes Custom (not under test)

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
    CHECK(!image_collection_clean_url(out, sizeof(out), "https://example.com/A.WEBP?x=1"), "the extension test is case-sensitive");
    CHECK(image_collection_clean_url(out, sizeof(out), "https://example.com/a.webp?x=1&y=2"), "a query");
    CHECK(image_collection_clean_url(out, sizeof(out), "example.com/a.png"), "isURL: the scheme is optional");
    CHECK(image_collection_clean_url(out, sizeof(out), "http://10.0.0.1:8080/a.gif#top"), "isURL: an IPv4 host, port and fragment");
    CHECK(!image_collection_clean_url(out, sizeof(out), "https://example.com/a.svg"), "svg refused");
    CHECK(!image_collection_clean_url(out, sizeof(out), "https://example.com/a.svg#.png"), "svg refused even with a png part");
    CHECK(!image_collection_clean_url(out, sizeof(out), "https://example.com/page.html"), "not an image refused");
    CHECK(!image_collection_clean_url(out, sizeof(out), "javascript:alert(1)//.png"), "not a URL");
    CHECK(!image_collection_clean_url(out, sizeof(out), "https://-bad.com/a.png"), "a label starts alphanumeric");
    CHECK(!image_collection_clean_url(out, sizeof(out), "https://e.c/a.png"), "a two-letter TLD at least");
    CHECK(!image_collection_clean_url(out, sizeof(out), "https://e.com/a b.png"), "no spaces");
    CHECK(image_collection_clean_url(out, sizeof(out), "https://e.com/\"a'.png") && !strcmp(out, "https://e.com/a.png"), "quotes dropped");

    // open the collection for a setting: tab 0 shows the built-in image, no own images yet
    HtmlArg open[1] = {str("customGameOverlay")};
    image_collection_handle("imagePopup", NULL, open, 1);
    CHECK(!strcmp(g_popup_cls, "imagePop") && strstr(g_popup, "img/backgrounds/0.png") && strstr(g_popup, "Midnight Snow<span class='imageListType'>Default</span>"), "built-in image on tab 0");
    CHECK(strstr(g_popup, "<div class='imageListHeader'>Image Collection<div class='imageListAdd' onclick='window.imageNewPopup(&quot;customGameOverlay&quot;)'") != NULL, "v10's header");
    CHECK(strstr(g_popup, "<div class='imageTab tabAIMG' onmouseenter='playTick()'\n\t\tonclick='playSelect(0.1);imagePopup(&quot;customGameOverlay&quot;, 0)'>Backgrounds</div>") != NULL, "tab 0 active");
    CHECK(strstr(g_popup, ">Reticles</div>") != NULL, "v10's tab names");

    // the New Image form
    HtmlArg add_key[1] = {str("customGameOverlay")};
    image_collection_handle("window.imageNewPopup", NULL, add_key, 1);
    CHECK(!strcmp(g_popup_cls, "imageNewPop") && strstr(g_popup, ">New Image</div>") && strstr(g_popup, "onclick='window.addNewImage(&quot;customGameOverlay&quot;)'"), "the New Image form");

    // add through the form, onto tab 2: the popup comes back on tab 0
    g_in_name = "Red dot";
    g_in_url = "https://example.com/dot.png";
    g_in_cat = "2";
    image_collection_handle("window.addNewImage", NULL, add_key, 1);
    CHECK(image_collection_count() == 1 && strstr(g_store, "dot.png") && strstr(g_store, "Red dot"), "added and stored");
    CHECK(!strstr(g_popup, "dot.png") && strstr(g_popup, "img/backgrounds/0.png"), "back on tab 0");
    HtmlArg tab2[2] = {str("customGameOverlay"), num(2)};
    image_collection_handle("imagePopup", NULL, tab2, 2);
    CHECK(strstr(g_popup, "selectImage(-2, &quot;https://example.com/dot.png&quot;, &quot;customGameOverlay&quot;)") && strstr(g_popup, "removeImage(0, &quot;customGameOverlay&quot;)")
          && strstr(g_popup, "Red dot<span class='imageListType'>Custom</span>") && !strstr(g_popup, "img/backgrounds/0.png"), "tab 2 shows the new image, not the built-in");

    // a bad link or name is refused with the response popup and nothing is stored
    g_in_name = "Bad";
    g_in_url = "https://example.com/x.svg";
    image_collection_handle("addNewImage", NULL, add_key, 1);
    CHECK(image_collection_count() == 1 && !strcmp(g_popup_cls, "responsePop") && strstr(g_popup, ">Invalid Image</div>"), "svg add refused");
    g_in_name = "!!!";
    g_in_url = "https://example.com/x.png";
    image_collection_handle("addNewImage", NULL, add_key, 1);
    CHECK(image_collection_count() == 1 && strstr(g_popup, ">Invalid Name</div>"), "empty name refused");
    g_in_name = "";
    image_collection_handle("addNewImage", NULL, add_key, 1);
    CHECK(image_collection_count() == 1 && strstr(g_popup, ">Something Went Wrong</div>"), "missing name refused");

    // Select sets the setting to the image's link and closes the popup
    HtmlArg pick[3] = {num(-2), str("https://example.com/dot.png"), str("customGameOverlay")};
    image_collection_handle("selectImage", NULL, pick, 3);
    CHECK(!strcmp(g_set_key, "customGameOverlay") && !strcmp(g_set_val, "https://example.com/dot.png") && g_closed, "pick sets the setting");

    // the built-in pick
    g_closed = 0;
    HtmlArg pick0[3] = {num(0), str("null"), str("customGameOverlay")};
    image_collection_handle("selectImage", NULL, pick0, 3);
    CHECK(!strcmp(g_set_val, "img/backgrounds/0.png") && g_closed, "built-in pick");

    // the list survives a reload from storage, and a removed entry is gone
    image_collection_reload();
    CHECK(image_collection_count() == 1, "reloaded from storage");
    HtmlArg rm[2] = {num(0), str("customGameOverlay")};
    image_collection_handle("removeImage", NULL, rm, 2);
    image_collection_reload();
    CHECK(image_collection_count() == 0, "removed and saved");

    // a hand-edited store: rows are kept as stored (v10 draws a bad link as an empty image), a non-row is left out
    html_storage_set("krk_custImgs", "[[\"ok\",\"https://e.com/a.jpg\",9],[\"\",\"https://e.com/b.jpg\",0],[\"x\",\"ftp://e.com/c.png\",0],5]");
    image_collection_reload();
    CHECK(image_collection_count() == 3, "stored rows kept");
    image_collection_handle("imagePopup", NULL, open, 1);
    CHECK(strstr(g_popup, ">Custom<span class='imageListType'>Custom</span>") && strstr(g_popup, "src='' ") && strstr(g_popup, "selectImage(-2, &quot;null&quot;"), "a nameless row and a bad link");

    CHECK(image_collection_handle("openSocial", NULL, NULL, 0) == -1, "other names are not handled");
    if (!g_fail) printf("image_collection_test: OK\n");
    return g_fail;
}
