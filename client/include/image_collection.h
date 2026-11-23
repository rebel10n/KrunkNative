// The image collection popup of a URL setting's "collections" button (imagePopup): client/src/ui/image_collection.c.
#ifndef KRUNKNATIVE_IMAGE_COLLECTION_H
#define KRUNKNATIVE_IMAGE_COLLECTION_H

#include <html_dom.h>
#include <stddef.h>

// imagePopup / imageNewPopup / addNewImage / removeImage / selectImage; -1 = not ours.
int image_collection_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc);

// The page the popup lives on: the menu (NULL, the default: v10_popup_show, ui_settings_set_string, v10_popup_close) or
// another page's own popup and settings (the hub, social_ui.c: genericPopup, its setSetting, openSettings).
typedef struct {
    void (*show)(const char *cls, const char *html);
    void (*set)(const char *key, const char *url);
    void (*close)(void);
} ImageCollectionHost;
void image_collection_set_host(const ImageCollectionHost *host);

// The stored list ("krk_custImgs"): add (0 and *why, the popup's text, on a bad name / URL), remove, count, an entry's URL.
int image_collection_add(const char *name, const char *url, int tab, const char **why);
int image_collection_remove(int index);
int image_collection_count(void);
const char *image_collection_url(int index);
void image_collection_reload(void); // read storage again on next use (tests)

// The checks an entry passes: letters, digits, space, _ and - in names; a link (isURL) to a PNG / JPG / GIF / WebP / ICO,
// not an SVG, its quote marks dropped.
void image_collection_clean_name(char *out, size_t size, const char *in);
int image_collection_clean_url(char *out, size_t size, const char *in);
// v10's isURL (config-10.0.7.js:4102-4108): an optional http(s)://, a domain or IPv4 host, port, path, query, fragment.
int image_collection_is_url(const char *u);

#endif
