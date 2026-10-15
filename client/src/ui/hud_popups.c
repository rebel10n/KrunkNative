// The game-overlay text popups of Krunker v3.1.1 (91.js overlay `divs`, drawn on the
// #game-overlay canvas every frame): the kill/objective score "+N" (scoreText, gf()),
// "Checkpoint!" (checkText, gg()) and the medal banner (chalName, gm()/gl(): kill medals
// from "6", "Assist" from "10", mode medals from "am"). The canvas drawing becomes one
// absolutely positioned element per popup inside #inGameUI (the #uiBase box the overlay
// canvas maps to: its 2D context is scaled by the #uiBase scale, so canvas units are
// #uiBase px), restyled every frame from the same animation state:
//
//   animateText (fd1) / animateDiv (cvZ7): fontSize = scale = size, minScale = size * 0.4
//   when scaleSpeed, life, fade; per frame (delta = the loop's clamped delta, dltMx 33 ms):
//   draw (when life >= 0, display, !disabled, no kill card / menu / end screen), then
//   delay -= delta (display at <= 0) or scale -= scaleSpeed * delta down to minScale,
//   life -= delta (removed + callback at <= 0), opacity = life / fade over the last `fade` ms.
//
// Text is drawn with textBaseline "middle" + textAlign "center" at (left %, top % or px);
// a `shadow` strokes the text black with lineWidth = shadow at +1 px x first.
// main.css sets `* { color: #353535 }`: every text element gets its color itself.
//
// player-viewer WP-E part 2a: Krunker v7.1.4 medals (index.js io_addMedal / f2009,
// game-overlay.js animateMedalNew + medalPrefab). A medal whose name is in
// overlay.medalsList (56 names) is an icon medal: img/medals/badge_0 behind
// img/medals/medal_<index>.png, the name, "+score" and an "x<count>" when the same medal
// repeats while it is up (or queued); sound medal_<snd> at 0.08 with playMedals. Other texts
// (trigger popups, "Purchased", "KR Claimed", ...) and every medal with classicMedals keep the
// banner (animateMedal on chalName). config.medalAnim is 1800 ms in v7 (1000 in v3.1.1), and
// medals are not shown in competitive games. Settings: showMedals, classicMedals, playMedals,
// medalScale, medalOffX / medalOffY (x 10 %), medalSat / medalHue (a CSS filter on the medal,
// settings.js bundleMedalFilters). v7's "am" sends [index, score, sound, count]; KrunkNative's
// medal packets carry the name, which maps to the same index, and every v7 medal plays sound
// medal_0. Documented difference: v7 queues a new medal with count 1 whatever count it came
// with (io_addMedal pushes [.., 1]); KrunkNative keeps the count.
#include <ui_logic.h>
#include <html_dom.h>
#include <html_ui.h>
#include <render_settings.h>
#include <audio.h>
#include <cJSON.h>
#include <progression.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define POPUP_DELTA_MAX_MS 33.0     // config.dltMx = round(1000 / 30)
#define MEDAL_ANIM_MS 1800.0        // config.medalAnim (v7.1.4; 1000 in v3.1.1)
#define MEDAL_DELAY_MS 900.0        // config.medalDelay
#define ASSIST_SCORE 25             // config.assistScore
#define MEDAL_QUEUE_MAX 64          // name/score pairs waiting (gj)

typedef struct Popup Popup;
typedef void (*PopupCallback)(void);

struct Popup {
    const char *id;          // DOM id of the element we create
    // overlay object fields (91.js Q.scoreText / Q.checkText / Q.chalName)
    char color[32];
    double left_pct;         // left: percent of the #uiBase width
    double top;              // top: percent of the #uiBase height, or px when top_px
    int top_px;
    double line_height;      // chalName: text drawn at lineHeight / 2
    double rect_w, rect_h;   // chalName: the gradient rect (fillRect(-w / 2, 0, w, h))
    int shadow;              // strokeText lineWidth (0 = none)
    int disabled;
    // animateText / animateDiv state
    char text[192];          // a trigger popup ("am" [tct, null]) is up to 70 characters; translated texts are longer
    char text2[32];
    double font_size, scale, min_scale, scale_speed, life, fade, opacity, delay;
    int display;
    PopupCallback callback;
    int in_divs;             // in overlay.divs
    int order;               // push order into divs (draw order: the loop runs from the end)
    // DOM
    HtmlElem el, text_el, text2_el, rect_el;
    int shown;
    char last_style[256];
};

static Popup g_score = {.id = "knScoreText", .color = "#ffea78", .left_pct = 50.0, .top = 37.0};
static Popup g_check = {.id = "knCheckText", .color = "#40C4FF", .left_pct = 50.0, .top = 20.0};
static Popup g_medal = {.id = "knChalName", .color = "#fff", .left_pct = 50.0, .top = 40.0, .top_px = 1,
                        .line_height = 150.0, .rect_w = 517.0, .rect_h = 167.0};
static Popup *const POPUPS[] = {&g_score, &g_check, &g_medal};
#define POPUP_COUNT ((int) (sizeof(POPUPS) / sizeof(POPUPS[0])))

static int g_order_seq;
static int g_score_total;            // bN: accumulated while the "+N" is up
static struct {
    char text[192];                  // medal names, trigger popup texts (TRIGGER_TEXT_MAX), translated lines
    int score;
    int index;                       // overlay.medalsList index, -1 = a text (classic banner only)
    int sound;                       // medal_<sound>
    int count;
} g_medals[MEDAL_QUEUE_MAX];         // vA651: [name / index, score, sound, count]
static int g_medal_count;
static double g_last_ms = -1.0;

/* ------------------------------------------------------------ overlay functions */

static void popup_push(Popup *p) {
    if (!p->in_divs) {
        p->in_divs = 1;
        p->order = g_order_seq++;
    }
}

// animateText (fd1): ap, text, size, scaleSpeed, life, fade, delay, callback
static void popup_animate_text(Popup *p, const char *text, const double size, const double scale_speed, const double life,
                               const double fade, const double delay, const PopupCallback callback) {
    popup_push(p);
    p->delay = delay;
    if (!delay) p->display = 1;
    p->font_size = size;
    p->scale = size;
    p->min_scale = scale_speed ? size * 0.4 : size;
    snprintf(p->text, sizeof(p->text), "%s", text);
    p->text2[0] = 0;
    p->scale_speed = scale_speed;
    p->life = life;
    p->fade = fade;
    p->opacity = 1.0;
    p->callback = callback;
}

static void upper(char *s) {
    for (; *s; s++) {
        if (*s >= 'a' && *s <= 'z') *s = (char) (*s - 'a' + 'A');
    }
}

// animateDiv (cvZ7): text and text2 upper-cased, otherwise like animateText
static void popup_animate_div(Popup *p, const char *text, const char *text2, const double size, const double scale_speed,
                              const double life, const double fade, const double delay, const PopupCallback callback) {
    popup_animate_text(p, text, size, scale_speed, life, fade, delay, callback);
    upper(p->text);
    snprintf(p->text2, sizeof(p->text2), "%s", text2 ? text2 : "");
    upper(p->text2);
}

/* ------------------------------------------------------------ DOM */

static HtmlElem popup_text_el(const HtmlElem parent) {
    const HtmlElem el = html_dom_create("div", NULL);
    html_dom_set_style(el, "position", "absolute");
    html_dom_set_style(el, "left", "0");
    html_dom_set_style(el, "white-space", "nowrap");
    html_dom_set_style(el, "font-family", "gamefont");
    html_dom_set_style(el, "line-height", "1");
    html_dom_set_style(el, "transform", "translate(-50%, -50%)"); // textAlign center, textBaseline middle
    // textBaseline "middle" is the middle of the em box Chromium normalizes from the font's ascent +
    // descent: GameFont (FrVrKrunkerBold2.ttf, upm 1000, ascent 1125, descent 250) puts the baseline
    // (1125 - 250) / 1375 / 2 = 0.3182 em below the anchor. A line-height: 1 box centres the 1.375 em
    // content area instead, baseline (1.125 - 0.25) / 2 = 0.4375 em below: up by the difference.
    html_dom_set_style(el, "margin-top", "-0.1193em");
    html_dom_append(parent, el);
    return el;
}

static HtmlElem popup_element(Popup *p) {
    if (p->el) return p->el;
    const HtmlElem host = html_dom_by_id("inGameUI");
    if (!host) return 0;
    const HtmlElem el = html_dom_create("div", p->id);
    html_dom_set_style(el, "position", "absolute");
    html_dom_set_style(el, "width", "0");
    html_dom_set_style(el, "height", "0");
    html_dom_set_style(el, "pointer-events", "none");
    html_dom_set_display(el, "none");
    if (p->rect_w > 0.0) {
        char v[32];
        p->rect_el = html_dom_create("div", NULL);
        html_dom_set_style(p->rect_el, "position", "absolute");
        snprintf(v, sizeof(v), "%gpx", -p->rect_w / 2.0);
        html_dom_set_style(p->rect_el, "left", v);
        html_dom_set_style(p->rect_el, "top", "0");
        snprintf(v, sizeof(v), "%gpx", p->rect_w);
        html_dom_set_style(p->rect_el, "width", v);
        snprintf(v, sizeof(v), "%gpx", p->rect_h);
        html_dom_set_style(p->rect_el, "height", v);
        // the linear gradient over the rect's width: 0 / .4 / .6 / 1 stops (game-overlay chalName.rect)
        html_dom_set_style(p->rect_el, "background", "linear-gradient(to right, rgba(0,0,0,0) 0%, rgba(0,0,0,0.3) 40%, "
                                                     "rgba(0,0,0,0.3) 60%, rgba(0,0,0,0) 100%)");
        html_dom_append(el, p->rect_el);
    }
    p->text_el = popup_text_el(el);
    if (p->rect_h > 0.0) {
        p->text2_el = popup_text_el(el);
        html_dom_set_style(p->text2_el, "font-size", "34px");
        char v[32];
        snprintf(v, sizeof(v), "%gpx", p->rect_h - 26.0); // fillText(text2, 0, height - 26)
        html_dom_set_style(p->text2_el, "top", v);
    }
    html_dom_append(host, el);
    p->el = el;
    p->shown = 0;
    p->last_style[0] = 0;
    return el;
}

// strokeText(text, 1, y) with lineWidth = shadow, then fillText(text, 0, y)
static void popup_shadow_css(const int shadow, char *out, const size_t size) {
    if (shadow <= 0) {
        snprintf(out, size, "none");
        return;
    }
    const double r = shadow / 2.0;
    snprintf(out, size, "%gpx 0 #000, %gpx 0 #000, 1px %gpx #000, 1px %gpx #000, %gpx %gpx #000, %gpx %gpx #000, "
                        "%gpx %gpx #000, %gpx %gpx #000",
             1.0 + r, 1.0 - r, r, -r, 1.0 + r, r, 1.0 - r, -r, 1.0 + r, -r, 1.0 - r, r);
}

static void popup_render(Popup *p, const int visible) {
    const int show = visible && p->in_divs && p->life >= 0.0 && p->display && !p->disabled;
    if (!show) {
        if (p->el && p->shown) html_dom_set_display(p->el, "none");
        p->shown = 0;
        return;
    }
    if (!popup_element(p)) return;

    char style[512], left[24], top[24], font[24], opacity[24], z[16];
    snprintf(left, sizeof(left), "%g%%", p->left_pct);
    if (p->top_px) snprintf(top, sizeof(top), "%gpx", p->top);
    else snprintf(top, sizeof(top), "%g%%", p->top);
    snprintf(font, sizeof(font), "%.3fpx", p->font_size);
    snprintf(opacity, sizeof(opacity), "%.4f", p->opacity);
    snprintf(z, sizeof(z), "%d", 1000 - p->order % 1000); // the loop draws divs from the end: the first pushed ends on top
    snprintf(style, sizeof(style), "%s|%s|%s|%s|%s|%s|%s|%s|%d|%s", left, top, font, opacity, z, p->text, p->text2, p->color,
             p->shadow, client_render_settings()->popup_score_color);
    if (!p->shown || strcmp(style, p->last_style) != 0) {
        html_dom_set_style(p->el, "left", left);
        html_dom_set_style(p->el, "top", top);
        html_dom_set_style(p->el, "opacity", opacity);
        html_dom_set_style(p->el, "z-index", z);
        html_dom_set_style(p->text_el, "font-size", font);
        html_dom_set_style(p->text_el, "color", p->color);
        char y[24];
        snprintf(y, sizeof(y), "%gpx", p->line_height / 2.0); // fillText(text, 0, lineHeight / 2)
        html_dom_set_style(p->text_el, "top", y);
        char shadow[256];
        popup_shadow_css(p->shadow, shadow, sizeof(shadow));
        html_dom_set_style(p->text_el, "text-shadow", shadow);
        html_dom_set_text(p->text_el, p->text);
        if (p->text2_el) {
            html_dom_set_display(p->text2_el, p->text2[0] ? "block" : "none");
            html_dom_set_text(p->text2_el, p->text2);
            html_dom_set_style(p->text2_el, "color", g_score.color); // fillStyle = scoreText.color
        }
        snprintf(p->last_style, sizeof(p->last_style), "%s", style);
    }
    if (!p->shown) html_dom_set_display(p->el, "block");
    p->shown = 1;
}

// The update half of the divs loop (after drawing).
static void popup_step(Popup *p, const double dt) {
    if (!p->in_divs) return;
    if (p->delay > 0.0) {
        p->delay -= dt;
        if (p->delay <= 0.0) p->display = 1;
        return;
    }
    if (p->scale > p->min_scale) {
        p->scale -= p->scale_speed * dt;
        if (p->scale <= p->min_scale) p->scale = p->min_scale;
        p->font_size = p->scale;
    }
    p->life -= dt;
    if (p->life <= 0.0) {
        p->display = 0;
        p->in_divs = 0; // divs.splice
        const PopupCallback callback = p->callback;
        if (callback) callback();
    }
    if (p->fade && p->life <= p->fade) p->opacity = p->life / p->fade;
}

/* ------------------------------------------------------------ the game's callers */

static void score_done(void) { g_score_total = 0; } // bN = 0

void hud_popups_score(const int delta) {
    if (!delta) return;
    g_score_total += delta;
    char text[24];
    snprintf(text, sizeof(text), "%s%d", g_score_total > 0 ? "+" : "", g_score_total);
    const float scale = client_render_settings()->popup_score_scale;
    popup_animate_text(&g_score, text, (scale ? scale : 1.0f) * 100.0, 0.38, 1100.0, 100.0, 0.0, score_done); // scoreTextScale || 1
}

void hud_popups_checkpoint(void) {
    if (!client_render_settings()->show_medals) return;
    popup_animate_text(&g_check, I18N("popup.checkpoint", "Checkpoint!"), 75.0, 0.38, 1100.0, 100.0, 0.0, NULL);
}

// overlay.medalsList (game-overlay.js:141): the icon medals, img/medals/medal_<index>.png
// (shared/src/progression.c: the game counts the same names for the end screen, WP-E part 2c)
#define MEDALS g_progression_medals
#define MEDAL_COUNT PROGRESSION_MEDAL_COUNT

static int medal_index(const char *name) {
    for (int i = 0; i < MEDAL_COUNT; i++)
        if (!strcmp(MEDALS[i], name)) return i;
    return -1;
}

static int g_comp;   // Game.isComp: no medals

static int setting_on(const char *key, const int fallback) {
    const cJSON *v = ui_setting_value(key);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    if (cJSON_IsNumber(v)) return v->valuedouble != 0.0;
    if (cJSON_IsString(v) && v->valuestring) return !strcmp(v->valuestring, "true");
    return fallback;
}

static double setting_num(const char *key, const double fallback) {
    double v;
    return ui_setting_number(key, &v) ? v : fallback;
}

// overlay.medalPrefab (an icon medal on the overlay canvas, drawn by the divs loop)
static struct {
    int in_divs, display, index, count, sound;
    double life, fade, opacity, delay, zoom;
    char name[48], score[32];
    PopupCallback callback;
    HtmlElem el, badge, icon, name_el, score_el, count_el;
    int shown;
    char last[256];
} g_icon = {.index = -1};

static void medal_sound(const int sound) {
    if (!setting_on("playMedals", 1)) return; // medalPrefab.sound
    char name[24];
    snprintf(name, sizeof(name), "medal_%d", sound > 0 ? sound : 0);
    audio_play(name, 0.08f, 0, 1.0f);
}

// animateMedalNew(prefab, index, score, sound, count, life, fade, delay, callback)
static void medal_animate_icon(const int index, const char *score, const int sound, const int count, const double life,
                               const double fade, const double delay, const PopupCallback callback) {
    g_icon.in_divs = 1;
    g_icon.delay = delay;
    if (!delay) g_icon.display = 1;
    snprintf(g_icon.score, sizeof(g_icon.score), "%s", score ? score : "");
    upper(g_icon.score);
    snprintf(g_icon.name, sizeof(g_icon.name), "%s", MEDALS[index]);
    upper(g_icon.name);
    g_icon.index = index;
    g_icon.zoom = 1.5;
    g_icon.life = life;
    g_icon.fade = fade;
    g_icon.sound = sound;
    g_icon.opacity = 1.0;
    g_icon.count = count > 0 ? count : 1;
    g_icon.callback = callback;
    if (!delay) medal_sound(sound);
}

static void medal_show(double delay);

static void medal_done(void) {
    // vA651.splice(0, 1); f2009(0)
    if (g_medal_count > 0) {
        memmove(&g_medals[0], &g_medals[1], (size_t) (g_medal_count - 1) * sizeof(g_medals[0]));
        g_medal_count--;
    }
    medal_show(0.0);
}

// f2009(delay): only while showMedals is on (except the "Reward" / "Spent" texts); like v7
// the queue is then left as it is (later medals queue up behind it until the next spawn).
static void medal_show(const double delay) {
    if (g_medal_count <= 0) return;
    if (!client_render_settings()->show_medals && strcmp(g_medals[0].text, "Reward") && strcmp(g_medals[0].text, "Spent")) return;
    char text2[24] = "";
    if (g_medals[0].score) snprintf(text2, sizeof(text2), "%s%d", g_medals[0].score > 0 ? "+" : "", g_medals[0].score);
    if (!setting_on("classicMedals", 0) && g_medals[0].index >= 0) {
        medal_animate_icon(g_medals[0].index, text2[0] ? text2 : NULL, g_medals[0].sound, g_medals[0].count, MEDAL_ANIM_MS, 50.0,
                           delay, medal_done);
        return;
    }
    popup_animate_div(&g_medal, g_medals[0].text, text2[0] ? text2 : NULL, 130.0, 0.4, MEDAL_ANIM_MS, 100.0, delay, medal_done);
}

// io_addMedal: one medal (name, score, sound, count); an icon medal already up gains the count
// and its full life again, a queued one its count.
static void medal_add(const char *text, const int score, const int sound, const int count) {
    if (!text || !text[0]) return;
    const int index = medal_index(text);
    const int c = count > 0 ? count : 1;
    if (!setting_on("classicMedals", 0) && index >= 0) {
        if (g_icon.in_divs && g_icon.index == index) {
            g_icon.count += c;
            g_icon.life = MEDAL_ANIM_MS;
            return;
        }
        for (int i = 0; i < g_medal_count; i++) {
            if (g_medals[i].index == index) {
                g_medals[i].count += c;
                return;
            }
        }
    }
    if (g_medal_count >= MEDAL_QUEUE_MAX) return;
    snprintf(g_medals[g_medal_count].text, sizeof(g_medals[0].text), "%s", text);
    g_medals[g_medal_count].score = score;
    g_medals[g_medal_count].index = index;
    g_medals[g_medal_count].sound = sound;
    g_medals[g_medal_count].count = c; // v7 pushes 1 here (documented above)
    g_medal_count++;
}

// io_addMedal(list): not in competitive games; the first medal of an empty queue waits
// config.medalDelay.
void hud_popups_medals(const NetMedalPacket *medals, const int count) {
    if (g_comp) return;
    const int was_empty = g_medal_count == 0;
    for (int i = 0; i < count; i++) {
        char text[sizeof(medals[i].text) + 1];
        snprintf(text, sizeof(text), "%.*s", (int) sizeof(medals[i].text), medals[i].text);
        medal_add(text, medals[i].score, 0, 1);
    }
    if (was_empty) medal_show(MEDAL_DELAY_MS);
}

// "am" [text, null] of a Broadcast / Send Popup trigger: one medal without score.
void hud_popups_medal_text(const char *text) {
    if (g_comp || !text || !text[0]) return;
    const int was_empty = g_medal_count == 0;
    medal_add(text, 0, 0, 1);
    if (was_empty) medal_show(MEDAL_DELAY_MS);
}

void hud_popups_assist(void) {
    const NetMedalPacket assist = {.text = "Assist", .score = ASSIST_SCORE}; // io_getAssist: [31, config.assistScore, 0, 1]
    hud_popups_medals(&assist, 1);
}

void hud_popups_set_comp(const int comp) { g_comp = !!comp; }

/* ------------------------------------------------------------ icon medal DOM */

static HtmlElem medal_text_el(const HtmlElem parent) {
    const HtmlElem el = popup_text_el(parent);
    html_dom_set_style(el, "transform", "translate(-50%, -75%)"); // textAlign centred by hand, alphabetic baseline
    html_dom_set_style(el, "text-shadow", "1.5px 0 #000, -1.5px 0 #000, 0 1.5px #000, 0 -1.5px #000, "
                                          "1px 1px #000, -1px -1px #000, 1px -1px #000, -1px 1px #000"); // strokeText, lineWidth 3
    return el;
}

static HtmlElem medal_icon_element(void) {
    if (g_icon.el) return g_icon.el;
    const HtmlElem host = html_dom_by_id("inGameUI");
    if (!host) return 0;
    const HtmlElem el = html_dom_create("div", "knMedal");
    html_dom_set_style(el, "position", "absolute");
    html_dom_set_style(el, "width", "0");
    html_dom_set_style(el, "height", "0");
    html_dom_set_style(el, "pointer-events", "none");
    html_dom_set_style(el, "z-index", "1001");
    html_dom_set_display(el, "none");
    g_icon.badge = html_dom_create("img", NULL);
    html_dom_set_attr(g_icon.badge, "src", "./img/medals/badge_0.svg");
    html_dom_set_style(g_icon.badge, "position", "absolute");
    html_dom_append(el, g_icon.badge);
    g_icon.icon = html_dom_create("img", NULL);
    html_dom_set_style(g_icon.icon, "position", "absolute");
    html_dom_append(el, g_icon.icon);
    g_icon.name_el = medal_text_el(el);
    html_dom_set_style(g_icon.name_el, "color", "#fff");
    g_icon.score_el = medal_text_el(el);
    g_icon.count_el = medal_text_el(el);
    html_dom_append(host, el);
    g_icon.el = el;
    g_icon.shown = 0;
    g_icon.last[0] = 0;
    return el;
}

// The medal branch of the divs loop: the badge ((size + 10) * z + 2 * border), the icon
// (0.65 * size * z, offset (0, -5)), the name at size * z / 2 + marginName * scale, the score
// at + marginScore * scale and "x<count>" right of the centre (marginCount * scale), all at
// medalOffX * 10 % / medalOffY * 10 % with z = zoom * medalScale.
static void medal_icon_render(const int visible) {
    const int show = visible && g_icon.in_divs && g_icon.life >= 0.0 && g_icon.display;
    if (!show) {
        if (g_icon.el && g_icon.shown) html_dom_set_display(g_icon.el, "none");
        g_icon.shown = 0;
        return;
    }
    if (!medal_icon_element()) return;
    const double scale = setting_num("medalScale", 1.0);
    const double z = g_icon.zoom * scale, size = 130.0, border = 7.0;
    const double left = setting_num("medalOffX", 5.0) * 10.0, top = setting_num("medalOffY", 1.0) * 10.0;
    const double sat = setting_num("medalSat", 1.0), hue = setting_num("medalHue", 0.0);
    char filter[96] = "none";
    if (sat != 1.0 && hue != 0.0) snprintf(filter, sizeof(filter), "saturate(%g%%) hue-rotate(%gdeg)", sat * 100.0, hue * 3.6);
    else if (sat != 1.0) snprintf(filter, sizeof(filter), "saturate(%g%%)", sat * 100.0);
    else if (hue != 0.0) snprintf(filter, sizeof(filter), "hue-rotate(%gdeg)", hue * 3.6);
    char key[256];
    snprintf(key, sizeof(key), "%.4f|%.4f|%g|%g|%s|%d|%s|%s|%d|%s", z, g_icon.opacity, left, top, filter, g_icon.index, g_icon.name,
             g_icon.score, g_icon.count, client_render_settings()->popup_score_color);
    if (g_icon.shown && !strcmp(key, g_icon.last)) return;
    snprintf(g_icon.last, sizeof(g_icon.last), "%s", key);
    char v[64];
    snprintf(v, sizeof(v), "%g%%", left);
    html_dom_set_style(g_icon.el, "left", v);
    snprintf(v, sizeof(v), "%g%%", top);
    html_dom_set_style(g_icon.el, "top", v);
    snprintf(v, sizeof(v), "%.4f", g_icon.opacity);
    html_dom_set_style(g_icon.el, "opacity", v);
    html_dom_set_style(g_icon.el, "filter", filter);
    const double bs = (size + 10.0) * z + border * 2.0;
    snprintf(v, sizeof(v), "%.2fpx", -bs / 2.0);
    html_dom_set_style(g_icon.badge, "left", v);
    html_dom_set_style(g_icon.badge, "top", v);
    snprintf(v, sizeof(v), "%.2fpx", bs);
    html_dom_set_style(g_icon.badge, "width", v);
    html_dom_set_style(g_icon.badge, "height", v);
    const double is = size * 0.65 * z;
    snprintf(v, sizeof(v), "./img/medals/medal_%d.png", g_icon.index);
    html_dom_set_attr(g_icon.icon, "src", v);
    snprintf(v, sizeof(v), "%.2fpx", -is / 2.0);
    html_dom_set_style(g_icon.icon, "left", v);
    snprintf(v, sizeof(v), "%.2fpx", -is / 2.0 - 5.0);
    html_dom_set_style(g_icon.icon, "top", v);
    snprintf(v, sizeof(v), "%.2fpx", is);
    html_dom_set_style(g_icon.icon, "width", v);
    html_dom_set_style(g_icon.icon, "height", v);
    const char *score_color = g_score.color; // vVO311.scoreText.color
    snprintf(v, sizeof(v), "%.2fpx", z * 20.0);
    html_dom_set_style(g_icon.name_el, "font-size", v);
    snprintf(v, sizeof(v), "%.2fpx", size * z / 2.0 + 40.0 * scale);
    html_dom_set_style(g_icon.name_el, "top", v);
    html_dom_set_text(g_icon.name_el, g_icon.name);
    html_dom_set_display(g_icon.score_el, g_icon.score[0] ? "block" : "none");
    if (g_icon.score[0]) {
        snprintf(v, sizeof(v), "%.2fpx", z * 18.0);
        html_dom_set_style(g_icon.score_el, "font-size", v);
        snprintf(v, sizeof(v), "%.2fpx", size * z / 2.0 + 70.0 * scale);
        html_dom_set_style(g_icon.score_el, "top", v);
        html_dom_set_style(g_icon.score_el, "color", score_color);
        html_dom_set_text(g_icon.score_el, g_icon.score);
    }
    html_dom_set_display(g_icon.count_el, g_icon.count > 1 ? "block" : "none");
    if (g_icon.count > 1) {
        snprintf(v, sizeof(v), "x%d", g_icon.count);
        html_dom_set_text(g_icon.count_el, v);
        snprintf(v, sizeof(v), "%.2fpx", z * 25.0);
        html_dom_set_style(g_icon.count_el, "font-size", v);
        snprintf(v, sizeof(v), "%.2fpx", 30.0 * scale);
        html_dom_set_style(g_icon.count_el, "left", v);
        snprintf(v, sizeof(v), "%.2fpx", size * z / 2.0);
        html_dom_set_style(g_icon.count_el, "top", v);
        html_dom_set_style(g_icon.count_el, "color", score_color);
    }
    if (!g_icon.shown) html_dom_set_display(g_icon.el, "block");
    g_icon.shown = 1;
}

static void medal_icon_step(const double dt) {
    if (!g_icon.in_divs) return;
    if (g_icon.display) { // the draw shrinks the zoom: zoom -= zoomInc * delta / 16, down to 1
        g_icon.zoom -= 0.06 * (dt / 16.0);
        if (g_icon.zoom < 1.0) g_icon.zoom = 1.0;
    }
    if (g_icon.delay > 0.0) {
        g_icon.delay -= dt;
        if (g_icon.delay <= 0.0) {
            g_icon.display = 1;
            medal_sound(g_icon.sound);
        }
        return;
    }
    g_icon.life -= dt;
    if (g_icon.life <= 0.0) {
        g_icon.display = 0;
        g_icon.in_divs = 0;
        const PopupCallback callback = g_icon.callback;
        if (callback) callback();
    }
    if (g_icon.fade && g_icon.life <= g_icon.fade) g_icon.opacity = g_icon.life / g_icon.fade;
}

// "start" (the local player spawns): gj.length = 0 and overlay.reset() (every popup hidden,
// divs emptied, no callbacks: bN keeps its value).
void hud_popups_on_spawn(void) {
    g_medal_count = 0;
    for (int i = 0; i < POPUP_COUNT; i++) {
        POPUPS[i]->display = 0;
        POPUPS[i]->in_divs = 0;
    }
    g_icon.display = 0;
    g_icon.in_divs = 0;
}

// "init" (a new game): bN = 0.
void hud_popups_on_init(void) {
    g_score_total = 0;
}

// A fresh page: our elements are gone.
void hud_popups_page_reset(void) {
    for (int i = 0; i < POPUP_COUNT; i++) {
        POPUPS[i]->el = POPUPS[i]->text_el = POPUPS[i]->text2_el = POPUPS[i]->rect_el = 0;
        POPUPS[i]->shown = 0;
    }
    g_icon.el = g_icon.badge = g_icon.icon = g_icon.name_el = g_icon.score_el = g_icon.count_el = 0;
    g_icon.shown = 0;
    g_last_ms = -1.0;
}

// Per frame: `visible` = the overlay draws (in game, no kill card / menu / end screen).
void hud_popups_tick(const double now_ms, const int visible) {
    const ClientRenderSettings *rs = client_render_settings();
    // showPScore / scoreColor / scoreShadow / scoreOffX / scoreOffY .set()
    g_score.disabled = !rs->show_popup_score;
    if (rs->popup_score_color[0]) snprintf(g_score.color, sizeof(g_score.color), "%s", rs->popup_score_color);
    g_score.shadow = rs->popup_score_shadow ? 3 : 0;
    g_score.left_pct = rs->popup_score_off_x * 10.0;
    g_score.top = rs->popup_score_off_y * 10.0;

    double dt = g_last_ms >= 0.0 ? now_ms - g_last_ms : 0.0;
    g_last_ms = now_ms;
    if (dt < 0.0) dt = 0.0;
    if (dt > POPUP_DELTA_MAX_MS) dt = POPUP_DELTA_MAX_MS;

    // divs loop: draw, then step; from the end of the list (latest pushed first)
    for (int i = 0; i < POPUP_COUNT; i++) popup_render(POPUPS[i], visible);
    medal_icon_render(visible);
    medal_icon_step(dt);
    for (int i = POPUP_COUNT - 1; i >= 0; i--) popup_step(POPUPS[i], dt);
}
