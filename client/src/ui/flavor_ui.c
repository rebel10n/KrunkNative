// Krunker Strike menu branding (flavor_ui.h). Everything here is gated on the process flavor, so
// a FLAVOR_BASE build never changes the page.
#include <flavor_ui.h>
#include <flavor.h>    // flavor_get / flavor_key / flavor_display_name
#include <html_dom.h>  // the DOM-ops API (no JS in the native client)
#include <stdio.h>     // snprintf

void flavor_ui_apply(void) {
    const GameFlavor f = flavor_get();
    if (f == FLAVOR_BASE) return; // the base game: leave the menu byte-for-byte unchanged

    // Master theming hook: a body class the page CSS (ui/css/krunknative.css) hangs the brand's
    // palette (--kn-flavor-*), font and logo rules off. Keyed by flavor_key() so a future flavor
    // needs no change here: body.kn-flavor-strike, body.kn-flavor-<key>, ...
    const HtmlElem body = html_dom_body();
    if (body) {
        char cls[32];
        snprintf(cls, sizeof cls, "kn-flavor-%s", flavor_key(f));
        html_dom_set_class(body, cls, 1);
    }

    // The main-menu logo. #gameNameHolder holds the base #mainLogo image (Krunker's "KRUNKER"
    // wordmark + season art), which is wrong for a flavor; under the body class krunknative.css
    // hides #mainLogo (and #mainLogoFace) and sizes the <img> we add here: #knFlavorLogo, the
    // flavor's own logo art, ui/img/kn-<key>-logo.png (Strike: the real Krunker Strike FRVR logo,
    // 1024x348; the engine keeps the image's ratio from its CSS height). The brand text is only
    // the image's alt/title (flavor_display_name()): no text-only wordmark.
    const HtmlElem holder = html_dom_by_id("gameNameHolder");
    if (holder && !html_dom_by_id("knFlavorLogo")) {
        const HtmlElem logo = html_dom_create("img", "knFlavorLogo");
        if (logo) {
            char src[64];
            snprintf(src, sizeof src, "img/kn-%s-logo.png", flavor_key(f));
            html_dom_set_attr(logo, "class", "kn-flavor-logo");
            html_dom_set_attr(logo, "src", src);
            html_dom_set_attr(logo, "alt", flavor_display_name(f));
            html_dom_set_attr(logo, "title", flavor_display_name(f));
            html_dom_append(holder, logo);
        }
    }
}

void flavor_ui_set_loading(const int loading) {
    if (flavor_get() == FLAVOR_BASE) return;
    const HtmlElem body = html_dom_body();
    if (body) html_dom_set_class(body, "kn-flavor-loading", loading ? 1 : 0);
}
