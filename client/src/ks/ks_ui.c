// GAME.UI (doc/v7/krunkscript.md 3.1.9; v7 createKrunkScriptApiContext.js:1656-1847): DOM
// elements over the game on the HTML page through html_dom.h. Parents: "" / "game" ->
// #customGUIHolder (first child of #gameUI in the v7 page; created here when the page lacks
// it), "menu" -> #MENUDIV (absent from the v7.1.4 page, so such calls fail as in v7), any other
// unprotected id. Every element gets pointer-events: all and onclick="knScriptClick()", which
// reaches C through the page's inline-handler path (native engine and web shim alike) and
// becomes onDIVClicked(<id of the innermost element>) once per click (v7 stopPropagation).
#include "ks_client_internal.h"
#include <html_dom.h>
#include <html_ui.h>
#include <ui_logic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// API:3 protected ids: never parents, targets or new ids
static const char *const PROTECTED_IDS[] = {
    "aHolder", "aMerger", "aContainer", "krunkerio_728x90_1",
    "/21823819281/frvr-krunker_io-krunker-display-banner-krunkerio_728x90_1",
    "/21823819281/frvr-frvr-krunker-display-banner-krunkerio_728x90_1", "mapVote", "mapVoteD", "mapVoteU",
    "modVoteHold", "modVoteD", "modVoteU", "modVote", "mapInfoHolder", "tDecCheck", "tSubCheck", "tSubOffer",
    "tHeader0", "tHeader1", "tradeReady0", "tradeReady1", "tSubName", "tradeChatHolder", "tradeChatList",
    "tradeChatItem", "tradeTotal", "trd_", "offerVal", "trd_inv_", "tItemN", "tItemCnt", "tItemImg",
    "tItemSprayImg", "tItemHolder", "tSubHolder", "tDecOffer", "tradeList0", "tradeList1", "offerVal0",
    "offerVal1", "estOfferVal", "tDecName", "tTotal", "tListN", "tHolderR", "tHolderL", "invList1", "tList",
    "invList0", "jumpSIMG", "jumpSVID", "jumpSVIDHolder", "material-icons", "vote", "ot-sdk-btn-floating",
    "interactMsg", "signedInHeaderBar", "signedOutHeaderBar", "accName", "accPass", "accResp", "referralHeader",
};
// API:5 updateDIVText refuses text containing these (case-insensitive)
static const char *const FORBIDDEN_TEXT[] = {"pass", "login", "user", "register", "signin"};

#define KSC_HOLDER "customGUIHolder"
#define KSC_CLICK_HANDLER "knScriptClick"

typedef struct {
    char id[128];
    HtmlElem el;
} UiElem;

static struct {
    UiElem *elems;
    int count, cap;
    HtmlElem own_holder;     // #customGUIHolder created here (the page had none)
    uint64_t click_frame;    // frame of the last delivered click (one per click)
    int handler_set;
    int hide_crosshair;      // overlay.disableCrosshair
    int hide_default;        // game.ui.hidden
    int style_state;         // hidden elements are being enforced (ks_ui_late)
} g_ui;

static int is_protected(const char *id) {
    for (size_t i = 0; i < sizeof(PROTECTED_IDS) / sizeof(PROTECTED_IDS[0]); i++) {
        if (!strcmp(PROTECTED_IDS[i], id)) return 1;
    }
    return 0;
}

// UTILS.cssCheck (UT:1420-1430): after removing \ ' " `, reject import|canvas|url|expression|
// behavior|-moz-binding|javascript|exec (case-insensitive; "important" allowed). A non-string or
// empty value passes; a value that is empty after the removal fails (v7 returns "").
static int css_check(const char *css) {
    if (!css || !*css) return 1;
    char clean[2048];
    size_t n = 0;
    for (const char *p = css; *p && n + 1 < sizeof(clean); p++) {
        if (*p == '\\' || *p == '\'' || *p == '"' || *p == '`') continue;
        clean[n++] = (char) tolower((unsigned char) *p);
    }
    clean[n] = 0;
    if (!n) return 0;
    // "important" -> placeholder so its "import" does not match
    for (char *imp = strstr(clean, "important"); imp; imp = strstr(imp + 9, "important")) memcpy(imp, "IMPXRTANT", 9);
    static const char *const BAD[] = {"import", "canvas", "url", "expression", "behavior", "-moz-binding", "javascript", "exec"};
    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        if (strstr(clean, BAD[i])) return 0;
    }
    return 1;
}

// el.style[prop]: camelCase names (backgroundColor) as kebab-case; kebab-case accepted as-is.
static void css_prop_name(const char *prop, char *out, const size_t n) {
    size_t k = 0;
    for (const char *p = prop; *p && k + 2 < n; p++) {
        if (isupper((unsigned char) *p)) {
            out[k++] = '-';
            out[k++] = (char) tolower((unsigned char) *p);
        } else {
            out[k++] = *p;
        }
    }
    out[k] = 0;
}

static void registry_add(const char *id, const HtmlElem el) {
    if (g_ui.count == g_ui.cap) {
        const int cap = g_ui.cap ? g_ui.cap * 2 : 32;
        UiElem *grown = realloc(g_ui.elems, (size_t) cap * sizeof(UiElem));
        if (!grown) return;
        g_ui.elems = grown;
        g_ui.cap = cap;
    }
    UiElem *e = &g_ui.elems[g_ui.count++];
    snprintf(e->id, sizeof(e->id), "%s", id);
    e->el = el;
}

static void registry_rename(const HtmlElem el, const char *id) {
    for (int i = 0; i < g_ui.count; i++) {
        if (g_ui.elems[i].el == el) snprintf(g_ui.elems[i].id, sizeof(g_ui.elems[i].id), "%s", id);
    }
}

static int registry_has(const HtmlElem el) {
    for (int i = 0; i < g_ui.count; i++) {
        if (g_ui.elems[i].el == el) return 1;
    }
    return 0;
}

// The default parent. The v7 page has it; the v3.1.1 page does not: then it is created at the
// start of #gameUI, covering the viewport, transparent to the pointer (its children opt in).
static HtmlElem holder(void) {
    HtmlElem h = html_dom_by_id(KSC_HOLDER);
    if (h) return h;
    const HtmlElem game_ui = html_dom_by_id("gameUI");
    const HtmlElem parent = game_ui ? game_ui : html_dom_body();
    if (!parent) return 0;
    html_dom_insert_html(parent, HTML_INSERT_AFTER_BEGIN,
        "<div id=\"" KSC_HOLDER "\" style=\"position:absolute;left:0;top:0;width:100%;height:100%;pointer-events:none;\"></div>");
    h = html_dom_by_id(KSC_HOLDER);
    g_ui.own_holder = h;
    return h;
}

// parentId argument -> element (API:1680-1690): missing / empty / "game" -> customGUIHolder,
// "menu" -> MENUDIV; protected or missing -> 0.
static HtmlElem resolve_parent(KsScript *s, const KsValue v, char *name, const size_t n) {
    char buf[256];
    const char *p = ksc_is_str(v) ? ksc_str(s, v, buf, sizeof(buf)) : "";
    char lower[16];
    size_t k = 0;
    for (; p[k] && k + 1 < sizeof(lower); k++) lower[k] = (char) tolower((unsigned char) p[k]);
    lower[k] = 0;
    if (!*p || (!p[k] && !strcmp(lower, "game"))) {
        snprintf(name, n, KSC_HOLDER);
        return holder();
    }
    snprintf(name, n, "%s", (!p[k] && !strcmp(lower, "menu")) ? "MENUDIV" : p);
    if (is_protected(name)) return 0;
    return html_dom_by_id(name);
}

static void ensure_click_handler(void);

// addDIV / addImage body: the element `tag` with `id`, display by `visible`, the script's CSS.
static KsValue add_element(KsScript *s, const char *tag, const KsValue id_v, const KsValue visible_v, const KsValue style_v,
                           const KsValue parent_v, const char *src) {
    char parent_name[256];
    const HtmlElem parent = resolve_parent(s, parent_v, parent_name, sizeof(parent_name));
    if (!parent) return ks_undef();
    char id[128];
    ksc_str(s, id_v, id, sizeof(id)); // p += ""
    if (html_dom_by_id(id) || is_protected(id)) return ks_undef();
    char style_buf[2048] = "";
    const char *style = ksc_truthy(style_v) ? ksc_str(s, style_v, style_buf, sizeof(style_buf)) : "";
    if (style_v.tag == KS_STR && !css_check(style)) return ks_undef();
    if (style_v.tag != KS_STR) style = ""; // cssCheck(non-string) passes; cssText += it as a string is never valid CSS here

    ensure_click_handler();
    const HtmlElem el = html_dom_create(tag, id);
    if (!el) return ks_undef();
    // el.style.display = visible ? "" : "none"; el.style.pointerEvents = "all"; cssText += style
    char css[2300];
    // !important: the in-game page is pointer-transparent (krunknative.css `#uiBase * { pointer-events:
    // none !important }`); an inline important declaration still yields to a script's own later one
    snprintf(css, sizeof(css), "%spointer-events:all !important;%s", ksc_truthy(visible_v) ? "" : "display:none;", style);
    html_dom_set_attr(el, "style", css);
    html_dom_set_attr(el, "onclick", KSC_CLICK_HANDLER "()");
    if (src) html_dom_set_attr(el, "src", src);
    html_dom_append(parent, el);
    registry_add(id, el);
    return ks_str(s, id, strlen(id));
}

// addDIV(id, visible, style = "", parentId = "game")
static KsValue ui_add_div(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    return add_element(s, "div", ksc_arg(a, n, 0), ksc_arg(a, n, 1), ksc_arg(a, n, 2), ksc_arg(a, n, 3), NULL);
}

// addImage(assetID, id, visible, style = "", parentId = "game"): <img src = user asset
// <digits>/texture.png> (non-digits stripped; none -> fails). The missing-image fallback is
// ./img/noimg.png (v7 onerror).
static KsValue ui_add_image(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    char raw[64], digits[32];
    ksc_str(s, ksc_arg(a, n, 0), raw, sizeof(raw));
    size_t k = 0;
    for (const char *p = raw; *p && k + 1 < sizeof(digits); p++) {
        if (*p >= '0' && *p <= '9') digits[k++] = *p;
    }
    digits[k] = 0;
    if (!k) return ks_undef();
    char rel[96], src[512];
    snprintf(rel, sizeof(rel), "user-assets/%s/texture.png", digits);
    html_dom_asset_url(rel, src, sizeof(src));
#ifndef __EMSCRIPTEN__
    char *path = concat(assets_path(), rel);
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (f) fclose(f);
    else snprintf(src, sizeof(src), "img/noimg.png");
    free(path);
#endif
    return add_element(s, "img", ksc_arg(a, n, 1), ksc_arg(a, n, 2), ksc_arg(a, n, 3), ksc_arg(a, n, 4), src);
}

// moveDIV(id, parentId, newId = "")
static KsValue ui_move_div(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    char parent_name[256];
    const HtmlElem parent = resolve_parent(s, ksc_arg(a, n, 1), parent_name, sizeof(parent_name));
    if (!parent) return ks_undef();
    char id[128];
    ksc_str(s, ksc_arg(a, n, 0), id, sizeof(id));
    if (is_protected(id)) return ks_undef();
    const HtmlElem el = html_dom_by_id(id);
    if (!el) return ks_undef();
    char new_id[128] = "";
    if (ksc_is_str(ksc_arg(a, n, 2))) ksc_str(s, ksc_arg(a, n, 2), new_id, sizeof(new_id));
    if (new_id[0] && is_protected(new_id)) return ks_undef();
    if (new_id[0]) {
        html_dom_set_attr(el, "id", new_id);
        registry_rename(el, new_id);
    }
    html_dom_append(parent, el); // appendChild moves it
    const char *out = new_id[0] ? new_id : id;
    return ks_str(s, out, strlen(out));
}

// removeDIV(id): any unprotected element (v7)
static KsValue ui_remove_div(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    char id[128];
    ksc_str(s, ksc_arg(a, n, 0), id, sizeof(id));
    const HtmlElem el = html_dom_by_id(id);
    if (!el || is_protected(id)) return ks_undef();
    for (int i = 0; i < g_ui.count; i++) {
        if (g_ui.elems[i].el == el) g_ui.elems[i] = g_ui.elems[--g_ui.count];
    }
    html_dom_remove(el);
    return ks_undef();
}

// getProp(id, cssProp): el.style[prop] || ""
static KsValue ui_get_prop(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    char id[128];
    ksc_str(s, ksc_arg(a, n, 0), id, sizeof(id));
    const HtmlElem el = html_dom_by_id(id);
    if (!el || is_protected(id) || !ksc_is_str(ksc_arg(a, n, 1))) return ks_undef();
    char prop[128], name[160], value[1024];
    ksc_str(s, ksc_arg(a, n, 1), prop, sizeof(prop));
    if (!css_check(prop)) return ks_undef();
    css_prop_name(prop, name, sizeof(name));
    if (html_dom_get_style(el, name, value, sizeof(value)) < 0) value[0] = 0;
    return ks_str(s, value, strlen(value));
}

// updateDIV(id, cssProp, value): el.style[prop] = value (non-string / failing value -> "")
static KsValue ui_update_div(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    char id[128];
    ksc_str(s, ksc_arg(a, n, 0), id, sizeof(id));
    const HtmlElem el = html_dom_by_id(id);
    if (!el || is_protected(id) || !ksc_is_str(ksc_arg(a, n, 1))) return ks_undef();
    char prop[128], name[160], value[1024] = "";
    ksc_str(s, ksc_arg(a, n, 1), prop, sizeof(prop));
    if (!css_check(prop)) return ks_undef();
    if (ksc_is_str(ksc_arg(a, n, 2))) ksc_str(s, ksc_arg(a, n, 2), value, sizeof(value));
    if (!css_check(value)) value[0] = 0;
    css_prop_name(prop, name, sizeof(name));
    if (!strcmp(name, "display")) html_dom_set_display(el, value);
    else html_dom_set_style(el, name, value);
    return ks_undef();
}

static int contains_forbidden(const char *text) {
    char lower[2048];
    size_t k = 0;
    for (; text[k] && k + 1 < sizeof(lower); k++) lower[k] = (char) tolower((unsigned char) text[k]);
    lower[k] = 0;
    for (size_t i = 0; i < sizeof(FORBIDDEN_TEXT) / sizeof(FORBIDDEN_TEXT[0]); i++) {
        if (strstr(lower, FORBIDDEN_TEXT[i])) return 1;
    }
    return 0;
}

// updateDIVText(id, text): textContent (non-string -> "")
static KsValue ui_update_div_text(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    char id[128];
    ksc_str(s, ksc_arg(a, n, 0), id, sizeof(id));
    const HtmlElem el = html_dom_by_id(id);
    if (!el || is_protected(id)) return ks_undef();
    const KsValue text_v = ksc_arg(a, n, 1);
    size_t len = 0;
    const char *text = ksc_is_str(text_v) ? ks_str_utf8(text_v, &len) : "";
    if (contains_forbidden(text)) return ks_undef();
    html_dom_set_text(el, text);
    return ks_undef();
}

// getDIVText(id): textContent ("menuWindow" refused, API:4)
static KsValue ui_get_div_text(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    char id[128];
    ksc_str(s, ksc_arg(a, n, 0), id, sizeof(id));
    if (is_protected(id) || !strcmp(id, "menuWindow")) return ks_undef();
    const HtmlElem el = html_dom_by_id(id);
    if (!el) return ks_undef();
    const int need = html_dom_get_text(el, NULL, 0);
    char *buf = malloc((size_t) (need > 0 ? need : 0) + 1);
    if (!buf) return ks_undef();
    buf[0] = 0;
    html_dom_get_text(el, buf, (size_t) (need > 0 ? need : 0) + 1);
    const KsValue out = ks_str(s, buf, strlen(buf));
    free(buf);
    return out;
}

// getSize(): {width, height} = window.innerWidth / innerHeight (CSS px)
static KsValue ui_get_size(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u; (void) a; (void) n;
    float w = 0, h = 0, dpr = 1;
    html_dom_viewport(&w, &h, &dpr);
    if ((w <= 0 || h <= 0) && g_ksc.client && g_ksc.client->window) {
        int ww, wh;
        glfwGetWindowSize(g_ksc.client->window, &ww, &wh);
        w = (float) ww;
        h = (float) wh;
    }
    return ksc_xy(s, "width", w, "height", h);
}

static KsValue ui_hide_crosshair(KsScript *s, void *u, const KsValue *a, int n) {
    (void) s; (void) u; (void) a; (void) n;
    g_ui.hide_crosshair = 1;
    ks_ui_late();
    return ks_undef();
}

// hideDefault(): game.ui.hidden = true; gameUIHider / fullMenHider display none
static KsValue ui_hide_default(KsScript *s, void *u, const KsValue *a, int n) {
    (void) s; (void) u; (void) a; (void) n;
    g_ui.hide_default = 1;
    ks_ui_late();
    return ks_undef();
}

void ks_ui_api(KsApiImpl *api) {
    api[KS_API_UI_ADDDIV].fn = ui_add_div;
    api[KS_API_UI_ADDIMAGE].fn = ui_add_image;
    api[KS_API_UI_MOVEDIV].fn = ui_move_div;
    api[KS_API_UI_REMOVEDIV].fn = ui_remove_div;
    api[KS_API_UI_GETPROP].fn = ui_get_prop;
    api[KS_API_UI_UPDATEDIV].fn = ui_update_div;
    api[KS_API_UI_UPDATEDIVTEXT].fn = ui_update_div_text;
    api[KS_API_UI_GETDIVTEXT].fn = ui_get_div_text;
    api[KS_API_UI_GETSIZE].fn = ui_get_size;
    api[KS_API_UI_HIDECROSSHAIR].fn = ui_hide_crosshair;
    api[KS_API_UI_HIDEDEFAULT].fn = ui_hide_default;
}

// ---------------------------------------------------------------------------
// Clicks

// onclick="knScriptClick()": ev->target is the element whose handler runs. Handlers of a
// script element and its script ancestors all run for one click (bubbling); the first one is
// the innermost element, the rest of the same frame are that click's bubbling (v7 stops it).
static int h_script_click(void *user, const HtmlEvent *ev, const HtmlArg *args, int argc) {
    (void) user; (void) args; (void) argc;
    if (!ev || !ev->target) return 1;
    if (g_ui.click_frame == g_ksc.frame + 1) return 1;
    char id[128];
    if (html_dom_get_attr(ev->target, "id", id, sizeof(id)) < 0) id[0] = 0;
    g_ui.click_frame = g_ksc.frame + 1;
    ks_ui_click(id);
    return 1;
}

static void ensure_click_handler(void) {
    if (g_ui.handler_set) return;
    g_ui.handler_set = 1;
    html_ui_add_handler(KSC_CLICK_HANDLER, h_script_click);
}

void ks_ui_click(const char *id) {
    KsScript *s = ksc_script();
    if (!s || !ksc_has_hook("onDIVClicked")) return;
    const KsValue arg = ks_str(s, id ? id : "", id ? strlen(id) : 0);
    ksc_call("onDIVClicked", &arg, 1);
}

// The element under a CSS point is a script element (or inside one).
int ks_ui_point_on_script_element(const float css_x, const float css_y) {
    for (HtmlElem el = html_dom_element_at(css_x, css_y); el; el = html_dom_parent(el)) {
        if (registry_has(el)) return 1;
        char id[16];
        if (html_dom_get_attr(el, "id", id, sizeof(id)) >= 0 && !strncmp(id, "cusDIV", 6)) return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Frame / lifecycle

int ks_ui_crosshair_hidden(void) { return g_ui.hide_crosshair; }

// hideCrosshair / hideDefault: the HUD code writes inline display values whenever its state
// changes, so the hidden elements are re-checked after the HUD pass every frame
// (ks_client_ui_late) with an important inline display (the native engine reads no <style>
// inserted at run time).
static const char *const DEFAULT_UI_IDS[] = {"gameUIHider", "fullMenHider", "inGameUI"};
static const char *const CROSSHAIR_IDS[] = {"knCrosshair", "aimRecticle", "aimDot"};

static void enforce_hidden(const char *const *ids, const size_t n, const int hide) {
    for (size_t i = 0; i < n; i++) {
        const HtmlElem el = html_dom_by_id(ids[i]);
        if (!el) continue;
        char v[64] = "";
        const int has = html_dom_get_style(el, "display", v, sizeof(v)) >= 0;
        const int hidden = has && !strncmp(v, "none", 4) && strstr(v, "important");
        if (hide && !hidden) html_dom_set_style(el, "display", "none !important");
        else if (!hide && hidden) html_dom_set_style(el, "display", "");
    }
}

void ks_ui_frame(void) {
    // our fallback holder follows the in-game UI (v7 toggleControlUI: shown while playing)
    if (g_ui.own_holder) {
        const int show = g_ksc.client && !html_ui_is_menu_open();
        html_dom_set_display(g_ui.own_holder, show ? "" : "none");
    }
}

void ks_ui_late(void) {
    const int state = (g_ui.hide_crosshair ? 1 : 0) | (g_ui.hide_default ? 2 : 0);
    if (!state && !g_ui.style_state) return;
    enforce_hidden(CROSSHAIR_IDS, sizeof(CROSSHAIR_IDS) / sizeof(CROSSHAIR_IDS[0]), g_ui.hide_crosshair);
    enforce_hidden(DEFAULT_UI_IDS, sizeof(DEFAULT_UI_IDS) / sizeof(DEFAULT_UI_IDS[0]), g_ui.hide_default);
    g_ui.style_state = state;
}

// overlay.resetAll (every INIT): #customGUIHolder emptied; the map GUI elements (#cusDIV<i>,
// re-created by the round setup) stay.
void ks_ui_round_reset(void) {
    const HtmlElem h = html_dom_by_id(KSC_HOLDER);
    if (!h) return;
    HtmlElem child = html_dom_first_child(h);
    while (child) {
        const HtmlElem next = html_dom_next(child);
        char id[16];
        if (html_dom_get_attr(child, "id", id, sizeof(id)) < 0 || strncmp(id, "cusDIV", 6) != 0) html_dom_remove(child);
        child = next;
    }
}

void ks_ui_unload(void) {
    for (int i = g_ui.count - 1; i >= 0; i--) {
        // the element may be gone already (removeDIV, a parent removed): only ours by identity
        if (g_ui.elems[i].id[0] && html_dom_by_id(g_ui.elems[i].id) == g_ui.elems[i].el) html_dom_remove(g_ui.elems[i].el);
    }
    g_ui.count = 0;
    g_ui.hide_crosshair = 0;
    g_ui.hide_default = 0;
    ks_ui_late(); // shows them again
}
