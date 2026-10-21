// Krunker Strike's first-time user experience (strike_ftue.h). Sources: FRVR Krunker Strike 1.37.2,
// assets/index-Cz2uYXJQ.deob.js (read as text; the names are its identifiers):
//   Gce.hasAlreadyPlayedBefore  ftue_already_played_before: false the first time (then stored true);
//                               the boot gate (c_e, ~line 154624) sends a first run on a touch device
//                               to quickConnect, anyone else to the main menu (lwe)
//   Qce                         the flows: start, nextStep, end, hasCompleted; a finished flow is
//                               recorded in __frvr_krunker_ftue_system, a step in __frvr_ftue_steps_done
//   CAe                         the overlay (.ftue-overlay, ~line 111555-111830): buildHighlightElement
//                               (four blockers around the element, the highlight box, the hand at the
//                               corner the element's centre is nearest to), buildImageElement,
//                               buildBlur, buildBottomText, buildBlockInputElement, the tutor (Q5 and
//                               the speech bubble eM)
// The controls tutorial (FRVR nB, the view below: its tap blur, hands, ripple, arrows, reload highlight and
// "Let's Go!") runs its steps in strike_ftue_controls.c.
#include <account_split.h> // the krunker.io identity counts only on an official game (account_split.h)
#include <strike_ui.h>
#include <strike_ftue.h>
#include <krunker_account.h>
#include <html_dom.h>
#include <ui_logic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FTUE_STORE_KEY "knFtue"  // the local FRVR __frvr_* entries (see the header)
#define FTUE_BLOCK_MARGIN 10.0f  // CAe's t: the gap between the element and its blockers
#define FTUE_BASE_W 640.0f       // the base layout when the overlay is not laid out yet
#define FTUE_BASE_H 360.0f

// ---------------------------------------------------------------- storage

static const char *account_key(void) {
    const char *name = krunker_account_name();
    return account_split_krunker_identity() && name && name[0] ? name : "guest";
}

static cJSON *load_store(void) {
    cJSON *root = NULL;
    const int size = html_storage_get(FTUE_STORE_KEY, NULL, 0);
    if (size > 0) {
        char *buf = malloc((size_t) size + 1);
        if (buf) {
            html_storage_get(FTUE_STORE_KEY, buf, (size_t) size + 1);
            root = cJSON_Parse(buf);
            free(buf);
        }
    }
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        root = cJSON_CreateObject();
    }
    return root;
}

static void save_store(cJSON *root) {
    char *text = cJSON_PrintUnformatted(root);
    if (text) {
        html_storage_set(FTUE_STORE_KEY, text);
        free(text);
    }
}

// root[account], created when missing
static cJSON *account_entry(cJSON *root) {
    const char *key = account_key();
    cJSON *entry = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsObject(entry)) entry = cJSON_AddObjectToObject(root, key);
    return entry;
}

int strike_ftue_first_run(void) {
    cJSON *root = load_store();
    cJSON *entry = account_entry(root);
    const int first = !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(entry, "played"));
    if (first) {
        cJSON_DeleteItemFromObjectCaseSensitive(entry, "played");
        cJSON_AddTrueToObject(entry, "played");
        save_store(root);
        printf("[strike] ftue: first run for %s, quick connect\n", account_key());
    }
    cJSON_Delete(root);
    return first;
}

static cJSON *flows_entry(cJSON *root) {
    cJSON *entry = account_entry(root);
    cJSON *flows = cJSON_GetObjectItemCaseSensitive(entry, "flows");
    if (!cJSON_IsObject(flows)) flows = cJSON_AddObjectToObject(entry, "flows");
    return flows;
}

int strike_ftue_completed(const char *flow) {
    cJSON *root = load_store();
    const cJSON *step = cJSON_GetObjectItemCaseSensitive(flows_entry(root), flow);
    const int done = cJSON_IsString(step) && !strcmp(step->valuestring, "done");
    cJSON_Delete(root);
    return done;
}

// The step is recorded (its id) unless the flow already ended.
void strike_ftue_step_done(const char *flow, const char *step) {
    cJSON *root = load_store();
    cJSON *flows = flows_entry(root);
    const cJSON *now = cJSON_GetObjectItemCaseSensitive(flows, flow);
    if (!(cJSON_IsString(now) && !strcmp(now->valuestring, "done"))) {
        cJSON_DeleteItemFromObjectCaseSensitive(flows, flow);
        cJSON_AddStringToObject(flows, flow, step);
        save_store(root);
    }
    cJSON_Delete(root);
}

void strike_ftue_end(const char *flow) {
    cJSON *root = load_store();
    cJSON *flows = flows_entry(root);
    cJSON_DeleteItemFromObjectCaseSensitive(flows, flow);
    cJSON_AddStringToObject(flows, flow, "done");
    save_store(root);
    cJSON_Delete(root);
}

// ---------------------------------------------------------------- overlay (CAe)

static struct {
    HtmlElem root;             // the page's .ftue-overlay
    char highlight[192];       // the element's selector, "" none
    int highlight_on, interactable;
    HtmlRect last;             // the highlight's box as last rendered
    int blur, block_input;
    char bottom[512];          // "" none
    char tutor[512];           // "" none
    int tutor_left, tutor_check;
    char image[256];           // "" none
    int image_flipped;
} F;

static int highlight_rect(HtmlRect *out) {
    if (!F.highlight_on || !F.highlight[0]) return 0;
    const HtmlElem el = html_dom_query(0, F.highlight);
    return el && html_dom_get_rect(el, out);
}

// CAe's hand: the corner the element's centre is nearest to points at the element (x, y, degrees)
static void hand_pose(const HtmlRect *e, const float vw, const float vh, float *x, float *y, float *deg) {
    const float t = FTUE_BLOCK_MARGIN;
    const float cx = e->x + e->w / 2, cy = e->y + e->h / 2;
    const float left = e->x - t, right = e->x + e->w + t, top = e->y - t, bottom = e->y + e->h + t;
    *x = cx;
    *y = 0;
    *deg = 0;
    if (cy < 100 && cx < 100) {
        *x = right;
        *y = bottom;
        *deg = 135;
    } else if (cy < 100 && cx > vw - 100) {
        *x = left;
        *y = bottom;
        *deg = 225;
    } else if (cy > vh - 100 && cx < 100) {
        *x = right;
        *y = top;
        *deg = 45;
    } else if (cy > vh - 100 && cx > vw - 100) {
        *x = left;
        *y = top;
        *deg = 315;
    } else if (cy > vh / 2) {
        *y = top;
        *deg = 0;
    } else {
        *y = e->y + e->h + t;
        *deg = 180;
    }
}

static void append_box(StrBuf *sb, const char *cls, float top, float left, float w, float h) {
    char b[320];
    snprintf(b, sizeof(b), "<div class=\"%s\" style=\"top:%.1fpx;left:%.1fpx;width:%.1fpx;height:%.1fpx\"></div>",
             cls, top, left, w < 0 ? 0 : w, h < 0 ? 0 : h);
    sb_append(sb, b);
}

// ---------------------------------------------------------------- controls view (nB)

// The controls tutorial's view (FRVR CONTROLS_FTUE nB, bundle ~110515-110550): the tap blur, the tutor
// (Q5 with the bubble eM), the touch hands (wF), the middle image, the ripple, the look arrows, the reload
// highlight, "Let's Go!" and the bottom text. It replaces CAe's overlay while on (strike_ftue_controls.c).
static struct {
    int on, blur, ignore, tutor_left, tutor_check, middle_flipped, ripple, arrows, reload_hl, complete;
    char bottom[512], tutor[512], middle[256], hand_side[8], hand_anim[12];
} CV;

static void render(void);

static void append_tutor(StrBuf *sb, const char *html, const int left, const int check) {
    char t[1100];
    const char *side = left ? "left" : "right";
    if (left) {
        snprintf(t, sizeof(t),
                 "<div class=\"ftue-controls__tutor-container ftue-controls__tutor-container-%s\">"
                 "<img class=\"ftue-controls__tutor ftue-controls__flipped\" src=\"./img/ftue/controls/tutor.png\">"
                 "<div id=\"mobSpeechBubble\" class=\"speech-bubble-container\"><div class=\"speech-bubble speech-bubble-ftue-controls speech-bubble-default\">%s</div></div>",
                 side, html);
    } else {
        snprintf(t, sizeof(t),
                 "<div class=\"ftue-controls__tutor-container ftue-controls__tutor-container-%s\">"
                 "<div id=\"mobSpeechBubble\" class=\"speech-bubble-container\"><div class=\"speech-bubble speech-bubble-ftue-controls speech-bubble-flipped\">%s</div></div>"
                 "<img class=\"ftue-controls__tutor\" src=\"./img/ftue/controls/tutor.png\">",
                 side, html);
    }
    sb_append(sb, t);
    if (check) {
        snprintf(t, sizeof(t), "<img class=\"ftue-controls__tutor--checkmark-%s\" src=\"./img/ftue/controls/checkmark.svg\">", side);
        sb_append(sb, t);
    }
    sb_append(sb, "</div>");
}

// wF: the hand's class follows its animation and side (vertical, horizontal or tap; the right side flips)
static void append_hand(StrBuf *sb, const char *anim) {
    const char *cls = "";
    if (!strcmp(anim, "vertical")) cls = " class=\"ftue-controls__touch-hand-vertical\"";
    else if (!strcmp(anim, "horizontal")) cls = " class=\"ftue-controls__touch-hand-horizontal-flipped\"";
    else if (!strcmp(anim, "tap")) cls = " class=\"ftue-controls__touch-hand-tap-flipped\"";
    char h[220];
    snprintf(h, sizeof(h), "<img%s src=\"./img/ftue/controls/hands_touch.svg\">", cls);
    sb_append(sb, h);
}

static void render_controls(void) {
    StrBuf sb = {NULL, 0, 0};
    char t[1100];
    sb_append(&sb, CV.ignore ? "<div class=\"ftue-controls__root ftue-controls__ignore-events\">" : "<div class=\"ftue-controls__root\">");
    if (CV.blur) sb_append(&sb, "<div class=\"frvrMenuBlur\" onclick=\"strikeAction('ftue-tap')\"></div>");
    if (CV.tutor[0]) append_tutor(&sb, CV.tutor, CV.tutor_left, CV.tutor_check);
    sb_append(&sb, "<div class=\"ftue-controls__top-left\"></div>");
    sb_append(&sb, "<div class=\"ftue-controls__left\">");
    if (CV.hand_anim[0] && !strcmp(CV.hand_side, "left")) append_hand(&sb, CV.hand_anim);
    sb_append(&sb, "</div>");
    sb_append(&sb, "<div class=\"ftue-controls__center\">");
    if (CV.middle[0]) {
        snprintf(t, sizeof(t), "<img class=\"ftue-controls__middle-image%s\" src=\"%s\">",
                 CV.middle_flipped ? " ftue-controls__flipped" : "", CV.middle);
        sb_append(&sb, t);
    }
    sb_append(&sb, "</div>");
    sb_append(&sb, "<div class=\"ftue-controls__right\">");
    if (CV.ripple) sb_append(&sb, "<div class=\"ripples\"><div class=\"ripple1\"></div><div class=\"ripple2\"></div><div class=\"ripple3\"></div></div>");
    if (CV.reload_hl) {
        sb_append(&sb, "<img class=\"ftue-controls__highlight-reload-animation\" src=\"./img/ftue/controls/highlight.png\">"
                       "<img class=\"ftue-controls__hand-pointer-reload-animation\" src=\"./img/ftue/controls/hands_pointer.svg\">");
    }
    if (CV.arrows) sb_append(&sb, "<img class=\"ftue-controls__arrows-horizontal\" src=\"./img/ftue/controls/arrows_horizontal.svg\">");
    if (CV.hand_anim[0] && !strcmp(CV.hand_side, "right")) append_hand(&sb, CV.hand_anim);
    if (CV.complete) {
        sb_append(&sb, "<div class=\"frvr-button frvr-button--yellow frvr-button--size-md frvr-button--width-150 frvr-animation-pulse ftue-controls__button\""
                       " onclick=\"strikeAction('ftue-go')\"><span>Let's Go!</span></div>"
                       "<img class=\"ftue-controls__hand-pointer-complete-animation\" src=\"./img/ftue/controls/hands_pointer.svg\">");
    }
    sb_append(&sb, "</div>");
    sb_append(&sb, "<div class=\"frvr-main-menu__bottom\"><div class=\"frvr-main-menu--bottom-center frvr-main-menu--bottom-center--tap-to-play\">");
    if (CV.bottom[0]) {
        snprintf(t, sizeof(t), "<span class=\"ftue-controls__tap-text\">%s</span>", CV.bottom);
        sb_append(&sb, t);
    }
    sb_append(&sb, "</div></div></div>");
    char *html = sb_finish(&sb);
    html_dom_set_html(F.root, html);
    free(html);
}

void strike_ftue_controls_view(const StrikeFtueControlsView *v) {
    if (!v) return;
    memset(&CV, 0, sizeof(CV));
    CV.on = 1;
    snprintf(CV.bottom, sizeof(CV.bottom), "%s", v->bottom ? v->bottom : "");
    snprintf(CV.tutor, sizeof(CV.tutor), "%s", v->tutor ? v->tutor : "");
    snprintf(CV.middle, sizeof(CV.middle), "%s", v->middle ? v->middle : "");
    snprintf(CV.hand_side, sizeof(CV.hand_side), "%s", v->hand_side ? v->hand_side : "");
    snprintf(CV.hand_anim, sizeof(CV.hand_anim), "%s", v->hand_anim ? v->hand_anim : "");
    CV.tutor_left = v->tutor_left;
    CV.tutor_check = v->tutor_check;
    CV.middle_flipped = v->middle_flipped;
    CV.blur = v->blur;
    CV.ignore = v->ignore_events;
    CV.ripple = v->ripple;
    CV.arrows = v->arrows;
    CV.reload_hl = v->reload_highlight;
    CV.complete = v->complete_button;
    render();
}

void strike_ftue_controls_view_clear(void) {
    memset(&CV, 0, sizeof(CV));
    render();
}

// CAe.view(): the elements in FRVR's order (blocker, middle image, blur, highlight, bottom text, tutor)
static void render(void) {
    if (!F.root) return;
    if (CV.on) {
        render_controls();
        return;
    }
    HtmlRect vp = {0, 0, FTUE_BASE_W, FTUE_BASE_H};
    html_dom_get_rect(F.root, &vp);
    if (vp.w <= 0) vp.w = FTUE_BASE_W;
    if (vp.h <= 0) vp.h = FTUE_BASE_H;
    StrBuf sb = {NULL, 0, 0};
    if (F.block_input) sb_append(&sb, "<div class=\"ftue-overlay__input-blocker\"></div>");
    if (F.image[0]) {
        char img[400];
        snprintf(img, sizeof(img), "<div class=\"ftue-overlay__center\"><img class=\"ftue-overlay__middle-image%s\" src=\"%s\"></div>",
                 F.image_flipped ? " ftue-overlay__flipped" : "", F.image);
        sb_append(&sb, img);
    }
    if (F.blur) sb_append(&sb, "<div class=\"ftue-overlay__dark-blur\"></div>");
    HtmlRect e;
    if (highlight_rect(&e)) {
        const float t = FTUE_BLOCK_MARGIN;
        append_box(&sb, "ftue-overlay__blocker", 0, 0, vp.w, e.y - t);
        append_box(&sb, "ftue-overlay__blocker", e.y - t, 0, e.x - t, e.h + 2 * t);
        append_box(&sb, "ftue-overlay__blocker", e.y - t, e.x + e.w + t, vp.w - e.x - e.w - t, e.h + 2 * t);
        append_box(&sb, "ftue-overlay__blocker", e.y + e.h + t, 0, vp.w, vp.h - e.y - e.h - t);
        append_box(&sb, F.interactable ? "ftue-overlay__highlight" : "ftue-overlay__highlight ftue-overlay__highlight--non-interactable",
                   e.y - t, e.x - t, e.w + 2 * t, e.h + 2 * t);
        if (F.interactable) {
            float hx, hy, hd;
            hand_pose(&e, vp.w, vp.h, &hx, &hy, &hd);
            char hand[300];
            snprintf(hand, sizeof(hand),
                     "<img class=\"ftue-overlay__highlight-hand\" src=\"./img/ftue/controls/hands_pointer.svg\""
                     " style=\"--hand-x:%.1fpx;--hand-y:%.1fpx;--hand-deg:%.1fdeg\">", hx, hy, hd);
            sb_append(&sb, hand);
        }
        F.last = e;
    }
    if (F.bottom[0]) {
        char b[600];
        snprintf(b, sizeof(b), "<div class=\"ftue-overlay__bottom-text\">%s</div>", F.bottom);
        sb_append(&sb, b);
    }
    if (F.tutor[0]) {
        // Q5: the tutor image and the speech bubble (eM), flipped on the left side
        const char *side = F.tutor_left ? "left" : "right";
        char t[1200];
        if (F.tutor_left) {
            snprintf(t, sizeof(t),
                     "<div class=\"ftue-controls__tutor-container ftue-controls__tutor-container-%s\">"
                     "<img class=\"ftue-controls__tutor ftue-controls__flipped\" src=\"./img/ftue/controls/tutor.png\">"
                     "<div id=\"mobSpeechBubble\" class=\"speech-bubble-container\"><div class=\"speech-bubble speech-bubble-ftue-controls speech-bubble-default\">%s</div></div>",
                     side, F.tutor);
        } else {
            snprintf(t, sizeof(t),
                     "<div class=\"ftue-controls__tutor-container ftue-controls__tutor-container-%s\">"
                     "<div id=\"mobSpeechBubble\" class=\"speech-bubble-container\"><div class=\"speech-bubble speech-bubble-ftue-controls speech-bubble-flipped\">%s</div></div>"
                     "<img class=\"ftue-controls__tutor\" src=\"./img/ftue/controls/tutor.png\">",
                     side, F.tutor);
        }
        sb_append(&sb, t);
        if (F.tutor_check) {
            char c[200];
            snprintf(c, sizeof(c), "<img class=\"ftue-controls__tutor--checkmark-%s\" src=\"./img/ftue/controls/checkmark.svg\">", side);
            sb_append(&sb, c);
        }
        sb_append(&sb, "</div>");
    }
    char *html = sb_finish(&sb);
    html_dom_set_html(F.root, html);
    free(html);
}

void strike_ftue_bind(const HtmlElem overlay) {
    F.root = overlay;
    render();
}

void strike_ftue_highlight(const char *selector, const int interactable) {
    snprintf(F.highlight, sizeof(F.highlight), "%s", selector ? selector : "");
    F.highlight_on = selector && selector[0];
    F.interactable = interactable;
    memset(&F.last, 0, sizeof(F.last));
    render();
}

void strike_ftue_unhighlight(void) {
    F.highlight_on = 0;
    render();
}

void strike_ftue_blur(const int on) {
    F.blur = on;
    render();
}

void strike_ftue_block_input(const int on) {
    F.block_input = on;
    render();
}

void strike_ftue_bottom_text(const char *html) {
    snprintf(F.bottom, sizeof(F.bottom), "%s", html ? html : "");
    render();
}

void strike_ftue_tutor(const char *html, const int left, const int check) {
    snprintf(F.tutor, sizeof(F.tutor), "%s", html ? html : "");
    F.tutor_left = left;
    F.tutor_check = check;
    render();
}

void strike_ftue_image(const char *src, const int flipped) {
    snprintf(F.image, sizeof(F.image), "%s", src ? src : "");
    F.image_flipped = flipped;
    render();
}

void strike_ftue_clear(void) {
    F.highlight_on = 0;
    F.blur = 0;
    F.block_input = 0;
    F.bottom[0] = 0;
    F.tutor[0] = 0;
    F.tutor_check = 0;
    F.image[0] = 0;
    render();
}

// The highlight follows its element (CAe's moving highlight, a 20 ms interval in FRVR): a new box
// is rendered when the element moved, appeared or resized.
void strike_ftue_tick(void) {
    HtmlRect e;
    if (!F.highlight_on || !highlight_rect(&e)) return;
    if (e.x != F.last.x || e.y != F.last.y || e.w != F.last.w || e.h != F.last.h) render();
}

// state.strike.ftue (--render-scenario)
void strike_ftue_scenario(const cJSON *ftue) {
    if (!ftue || !cJSON_IsObject(ftue)) return;
    strike_ftue_clear();
    const cJSON *hl = cJSON_GetObjectItemCaseSensitive(ftue, "highlight");
    if (cJSON_IsString(hl)) {
        strike_ftue_highlight(hl->valuestring, 1);
    } else if (cJSON_IsObject(hl)) {
        const cJSON *sel = cJSON_GetObjectItemCaseSensitive(hl, "selector");
        const cJSON *ia = cJSON_GetObjectItemCaseSensitive(hl, "interactable");
        if (cJSON_IsString(sel)) strike_ftue_highlight(sel->valuestring, !cJSON_IsFalse(ia));
    }
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(ftue, "blur"))) strike_ftue_blur(1);
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(ftue, "blocker"))) strike_ftue_block_input(1);
    const cJSON *bottom = cJSON_GetObjectItemCaseSensitive(ftue, "bottomText");
    if (cJSON_IsString(bottom)) strike_ftue_bottom_text(bottom->valuestring);
    const cJSON *tutor = cJSON_GetObjectItemCaseSensitive(ftue, "tutor");
    if (cJSON_IsObject(tutor)) {
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(tutor, "text");
        const cJSON *side = cJSON_GetObjectItemCaseSensitive(tutor, "side");
        strike_ftue_tutor(cJSON_IsString(text) ? text->valuestring : "", !(cJSON_IsString(side) && !strcmp(side->valuestring, "right")),
                          cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(tutor, "check")));
    }
    const cJSON *image = cJSON_GetObjectItemCaseSensitive(ftue, "image");
    if (cJSON_IsObject(image)) {
        const cJSON *src = cJSON_GetObjectItemCaseSensitive(image, "src");
        if (cJSON_IsString(src)) strike_ftue_image(src->valuestring, cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(image, "flipped")));
    }
}
