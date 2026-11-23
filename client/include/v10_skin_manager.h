// Krunker v10.0.7's Skin Manager on the v10 page (client/src/ui/v10_skin_manager.c): the sandbox menu's
// window (window.showSkinManager, deob-10.0.7.js:204459; component :124611-127559) on the shared view
// stack, over the /skins gapi routes (services/brain/skins.c).
#ifndef KRUNKNATIVE_V10_SKIN_MANAGER_H
#define KRUNKNATIVE_V10_SKIN_MANAGER_H

#include <html_dom.h>
#include <stddef.h>

int  v10_skin_manager_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc); // -1 = not ours
int  v10_skin_manager_key(int key);  // Escape on the window or its delete prompt: 1 = handled
void v10_skin_manager_tick(void);    // per frame: the search's and the preview's debounces, Test's pending skin swap
int  v10_skin_manager_open(void);    // 1 while the window is on the view stack (the client e2e)
// The URL of a custom skin's file (thumbnail.png, texture.png, ...) under skins/<folder>/, as userAssetsUrl builds it
// with ?v=<rev>: a Box Office skin's (user_asset_rel_is_kn) is the brain's /user-assets/, else user-assets.krunker.io.
void v10_skin_file_url(const char *folder, const char *file, unsigned rev, char *out, size_t n);

#endif
