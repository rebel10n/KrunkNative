// Krunker Strike's menu screens (strike_ui.h): FRVR 1.37.2's Mithril views. Their markup is the
// DOM FRVR's own code rendered (ui/strike/screens/<VIEW_ID>.html, tools/strike/strike_screens.py);
// the click hooks the generator put on it (onclick="strikeAction('<name>')") and the live values
// are handled here over the DOM-ops API. Sources (assets/index-Cz2uYXJQ.js, read as text):
//   O5 MAIN_MENU          the account box (jU: face, name, Level N, xp / next, tickets, KR), Extras,
//                         Customize, feedback, PLAY (quickConnect: "Loading ..." then the game), KR limit
//   B5 CLASS_SELECTION    ode (the class bullets) + lde (the selected one) + Select / Selected (_r is the
//                         previewed class; selectClass(_r) then kt())
//   Ly CUSTOMIZATION      the CLASS: <name> button -> CLASS_SELECTION
//   Dy SETTINGS           Presets / Display / Game / Sound / Other tabs (tabSelectorItem.selected),
//                         the preset buttons (frvr-preset__button--active)
//   Yw TAP_TO_PLAY        between lives when the player paused (Yh(false)): PLAY = Cg()
//   YC PROFILE, m5 CREATE_MATCH, _C BATTLEPASS_SCREEN: shown as FRVR renders them for a guest
//   fg STORE (Extras)     MAIN_MENU / TAP_TO_PLAY #mobMenuStoreH: Pt(cc()) (cc() is fg below level 5);
//                         Spins / Market / Currency (bce sectioned scroll); Spins = Sr(OC): one _I
//                         block (#wheelStoreBlock) per shop wheel, a click eb(id) = Pt(SA, {spinnerIndex})
//   SA SPIN               the wheel (aue: canvas sized by its parent, --base-scale F / 280 px), its odds
//                         (oU), the split 10x / 1x ticket button; back kt(), home kt() + the store's kt()
//   wb CUSTOMIZE_CONTROLS Settings > Game "Customize Controls": Pt(ZC, {onHide: kt}); back kt(), home
//                         onHide() + kt() (Settings goes too), Reset = Ahe() (the layout to Gw defaults)
//   In POPUP              the popup frame (Pt(In, {builder})): .frvr-dark-blur-75 dismisses (kt)
//   YB LEADERBOARD_PLAYERS FRVR hosts it only in its tournament rewards popup (qB, In); KrunkNative has no
//                         hosted tournaments, so only a scenario (state.strike.leaderboard) shows it
// The Strike classes are FRVR's six (tools/strike/strike_data.json "classes"): Krunker class ids
// 0 Triggerman, 1 Hunter, 2 Run N Gun, 4 Shotgunner (Krunker's Vince), 5 Detective, 7 Rocketeer.
#include <strike_ui.h>
#include <html_ui.h>
#include <ui_logic.h>
#include <backend_ui.h>
#include <spin_wheel.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int id;
    const char *name, *weapon, *description;
    int rarity;           // class-rarity-banner--N
    const char *slot_img; // the white weapon silhouette FRVR renders for the bullet (captured)
} StrikeClass;

static const StrikeClass CLASSES[] = {
    {0, "Triggerman", "Assault Rifle", "Reliable backbone of any encounter who is armed for combat at any range with a trusty Assault Rifle.", 0,
     "./img/kn-captured/ff80fd90b539.png"},
    {1, "Hunter", "Sniper Rifle", "Take aim with the elite marksman who turns every battlefield into a deadly game of cat and mouse. The hunter strikes fear from afar with the Sniper Rifle but struggles in close combat.", 3,
     "./img/kn-captured/af22e8914cbe.png"},
    {2, "Run N Gun", "Submachine Gun", "High-octane, speed demon of the battlefield that excels at fast paced close encounters. Equipped with a fast firing close range SMG.", 0,
     "./img/kn-captured/3e968dcb92ed.png"},
    {4, "Shotgunner", "Shotgun", "Close quarters high risk, high reward specialist who thrives on the front lines of any battle with a high-powered Shotgun.", 1,
     "./img/kn-captured/dd2ff88f9ecd.png"},
    {5, "Detective", "Revolver", "With a powerful six-shooter Revolver, the Detective delivers devastating damage with pinpoint accuracy in short to mid ranged combat.", 2,
     "./img/kn-captured/e06ab1f842fd.png"},
    {7, "Rocketeer", "Rocket Launcher", "Explosive-loving demolition expert who brings chaos into the battlefield. Causes massive area of effect damage with the Rocket Launcher.", 3,
     "./img/kn-captured/e05eb215c15c.png"},
};
#define STRIKE_CLASS_COUNT ((int) (sizeof(CLASSES) / sizeof(CLASSES[0])))

static const char *const SETTINGS_TABS[] = {"presets", "display", "game", "sound", "other"};

// The shop's prize wheels. FRVR serves them from its gateway (GET /v2/spin-wheels/list, the wheel
// itself /v1/spin-wheels/<id>); the 1.37.2 bundle and tools/strike/strike_data.json carry none, so
// these are the wheels of the sandbox that captured STORE / SPIN (tools/uidiff/capture-strike.js
// SPIN_WHEELS), in OC's order (KR wheels by price, highest first, then the ticket wheel). Their odds
// are the capture's too; the rarity names are FRVR's windows.spin.rarity.* strings.
typedef struct { const char *name, *color; double weight; } StrikeOdd;
static const StrikeOdd WHEEL_ODDS[] = {
    {"Common", "#a7b1c2", 0.5}, {"Uncommon", "#79d94a", 0.28}, {"Rare", "#3aa0ff", 0.14}, {"Epic", "#b45cff", 0.06}, {"Legendary", "#ffb800", 0.02},
};
#define WHEEL_ODD_COUNT ((int) (sizeof(WHEEL_ODDS) / sizeof(WHEEL_ODDS[0])))
typedef struct {
    const char *id, *name;
    int ticket; // price.currency spin_ticket (else kr)
    int amount; // price.amount
} StrikeWheel;
static const StrikeWheel WHEELS[] = {
    {"lucky-draw", "Lucky Draw", 0, 500},
    {"wheel-kr", "KR Spin", 0, 250},
    {"wheel-ticket", "Ticket Spin", 1, 1},
};
#define WHEEL_COUNT ((int) (sizeof(WHEELS) / sizeof(WHEELS[0])))
#define WHEEL_DEFAULT 2 // the ticket wheel (the SPIN capture's spinnerIndex)
// OC's grid spans per wheel count ("column/row")
static const char *const WHEEL_GRID[][8] = {
    {0}, {"12/2"}, {"6/2", "6/2"}, {"6/2", "6/1", "6/1"}, {"6/1", "6/1", "6/1", "6/1"},
};

#define BOARD_MAX 16
typedef struct {
    char name[32];
    int class_id, you;
    int has_stats, score, kills;
} BoardRow;

static struct {
    Client *client;
    int previewed;        // _r: the class bullet shown in CLASS_SELECTION
    int settings_tab;
    int preset;           // selectedPresetIndex (3 = Custom)
    char last_account[160];
    int spin;             // SA's spinnerIndex (a WHEELS index)
    int store_section;    // bce: the active section title
    SpinWheelDraw wheel;  // SPIN's canvas this frame (spin_wheel.c draws it into the page's canvas)
    float wheel_f;        // aue's F (the canvas side, CSS px) last applied
    // state.strike.leaderboard of a --render-scenario (the capture's rows), used when set
    int board_fixture;
    BoardRow board[BOARD_MAX];
    int board_count;
} M = {.preset = 3, .spin = WHEEL_DEFAULT};

// ---------------------------------------------------------------- helpers

static int class_slot(const int class_id) {
    for (int i = 0; i < STRIKE_CLASS_COUNT; i++) {
        if (CLASSES[i].id == class_id) return i;
    }
    return 0;
}

static int current_class(void) {
    return M.client ? class_slot(M.client->menu_class_index) : 0;
}

static void set_text(const HtmlElem root, const char *sel, const char *text) {
    const HtmlElem e = root ? html_dom_query(root, sel) : 0;
    if (e) html_dom_set_text(e, text);
}

// The state.strike.account fixture of a --render-scenario (strike_menu_scenario): FRVR's account box
// numbers for a capture, used only when set. The guest name is the menu's name (client->menu_name,
// the v10 page's name field) as the rest of the menu shows it.
static struct {
    int set;
    int level, xp, next_xp, tickets, kr;
    char name[32];
} FIXTURE;

static void account_values(char *name, size_t nn, int *level, int *kr) {
    const AccountInfo *acc = account_get();
    const Client *c = M.client;
    if (FIXTURE.set) {
        snprintf(name, nn, "%s", FIXTURE.name);
        *level = FIXTURE.level;
        *kr = FIXTURE.kr;
    } else if (acc) {
        snprintf(name, nn, "%s", acc->alias[0] ? acc->alias : acc->name);
        *level = acc->level > 0 ? acc->level : 1;
        *kr = (int) acc->funds;
    } else {
        snprintf(name, nn, "%s", c && c->menu_name[0] ? c->menu_name : "Guest");
        *level = 1;
        *kr = 0;
    }
}

void strike_menu_scenario(const cJSON *acc, const char *name) {
    if (!cJSON_IsObject(acc)) return;
    FIXTURE.set = 1;
    FIXTURE.level = (int) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(acc, "level"));
    FIXTURE.xp = (int) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(acc, "xp"));
    FIXTURE.next_xp = (int) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(acc, "nextXp"));
    FIXTURE.tickets = (int) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(acc, "tickets"));
    FIXTURE.kr = (int) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(acc, "kr"));
    snprintf(FIXTURE.name, sizeof(FIXTURE.name), "%s", name ? name : "Guest");
}

static void fill_account(const HtmlElem root) {
    char name[64];
    int level = 1, kr = 0;
    account_values(name, sizeof(name), &level, &kr);
    char buf[64];
    set_text(root, "#mobMenuAccName", name);
    snprintf(buf, sizeof(buf), "Level %d", level);
    set_text(root, ".account-level", buf);
    snprintf(buf, sizeof(buf), "%d", kr);
    HtmlElem cur[4];
    const int n = html_dom_query_all(root, ".account-currency span", cur, 4);
    if (n >= 2) html_dom_set_text(cur[1], buf); // [0] spin tickets, [1] KR
    if (n >= 1) html_dom_set_text(cur[0], FIXTURE.set ? (snprintf(buf, sizeof(buf), "%d", FIXTURE.tickets), buf) : "0");
    if (FIXTURE.set) {
        snprintf(buf, sizeof(buf), "%d", FIXTURE.xp);
        set_text(root, ".account-current-xp", buf);
        snprintf(buf, sizeof(buf), "/%d", FIXTURE.next_xp);
        set_text(root, ".account-xp-to-next-level", buf);
    }
    // the face: the selected class's icon (Se.account face / getClassIcon)
    const HtmlElem img = html_dom_query(root, "#mobMenuAccImg img");
    if (img) {
        snprintf(buf, sizeof(buf), "./cdn/textures/classes/icon_%d.png", CLASSES[current_class()].id);
        html_dom_set_attr(img, "src", buf);
    }
}

static int spin_tickets(void) { return FIXTURE.set ? FIXTURE.tickets : 0; } // Po(Se.account)

// The currency header of STORE / SPIN (FRVR's .currency-header, A1): spin tickets and KR as the account
// box shows them (the tickets are 0 on a fresh account, as fill_account does). Only the header's: SPIN's
// wheel buttons are .spin-tickets too (their prices).
static void fill_currency(const HtmlElem root) {
    char name[64], buf[32];
    int level = 1, kr = 0;
    account_values(name, sizeof(name), &level, &kr);
    HtmlElem spans[8];
    snprintf(buf, sizeof(buf), "%d", spin_tickets());
    int n = html_dom_query_all(root, ".currency-header .spin-tickets span", spans, 8);
    for (int i = 0; i < n; i++) html_dom_set_text(spans[i], buf);
    snprintf(buf, sizeof(buf), "%d", kr);
    n = html_dom_query_all(root, ".currency-header .krunkies span", spans, 8);
    for (int i = 0; i < n; i++) html_dom_set_text(spans[i], buf);
    // the "+" (nue / DI, showBuy {section}): spin tickets -> the Spin Wheel section (dm(Yn.SpinWheel):
    // Spins), KR -> the Currency Grid section (dm(Yn.CurrencyGrid): Currency)
    static const struct { const char *sel, *hook; } BUY[] = {
        {".currency-header .spin-tickets .purchase-more-btn", "strikeAction('buy:0')"},
        {".currency-header .krunkies .purchase-more-btn", "strikeAction('buy:2')"},
    };
    for (size_t b = 0; b < sizeof(BUY) / sizeof(BUY[0]); b++) {
        n = html_dom_query_all(root, BUY[b].sel, spans, 8);
        for (int i = 0; i < n; i++) html_dom_set_attr(spans[i], "onclick", BUY[b].hook);
    }
}

// ---------------------------------------------------------------- STORE (fg): Spins = Sr(OC)

// qf (sue): the spin ticket amount; outline unless outline:false, iconRight puts the image after
static void sb_ticket_amount(StrBuf *sb, const int amount, const int small, const int icon_right) {
    sb_appendf(sb, "<div class=\"spin-tickets %s spin-tickets--outline\">", small ? "spin-tickets--small" : "");
    if (icon_right) sb_appendf(sb, "<span>%d</span><img>", amount);
    else sb_appendf(sb, "<img><span>%d</span>", amount);
    sb_append(sb, "</div>");
}

// OC.view: one _I block per wheel (renderBackgroundImage: the wheel's imageUrl, none here), title r(p)
// (KR: the coin + amount, tickets: qf), subtitle the wheel's name, overlay l(p) (a ticket wheel: wce,
// the tickets owned), the click eb(p._id) -> SPIN
static char *store_wheels_html(void) {
    StrBuf sb = {0};
    for (int i = 0; i < WHEEL_COUNT; i++) {
        const StrikeWheel *w = &WHEELS[i];
        char col[8] = "", row[8] = "";
        const char *span = WHEEL_COUNT < (int) (sizeof(WHEEL_GRID) / sizeof(WHEEL_GRID[0])) ? WHEEL_GRID[WHEEL_COUNT][i] : "3/1";
        sscanf(span ? span : "3/1", "%7[^/]/%7s", col, row);
        sb_appendf(&sb, "<div id=\"wheelStoreBlock\" class=\"frvr-store-block-container store-wheel\" style=\"grid-column: span %s; grid-row: span %s;\""
                        " onclick=\"strikeAction('spin:%d')\"><div class=\"frvr-store-block\"><div class=\"frvr-store-block-title\">",
                   col, row, i);
        if (w->ticket) sb_ticket_amount(&sb, w->amount, 0, 0);
        else sb_appendf(&sb, "<img class=\"frvr-store-block-title__KRimg\" src=\"img/frvr-ui/icon-krunkie-coin.svg\"><span>%d</span>", w->amount);
        sb_append(&sb, "</div><div class=\"frvr-store-block-subtitle\">");
        sb_append_esc(&sb, w->name);
        sb_append(&sb, "</div>");
        if (w->ticket) {
            sb_append(&sb, "<div class=\"frvr-store-block-overlay\"><div class=\"frvr-store-wheels-overlay-mini-top-right\">");
            sb_ticket_amount(&sb, spin_tickets(), 1, 1);
            sb_append(&sb, "</div></div>");
        }
        sb_append(&sb, "</div></div>");
    }
    return sb_finish(&sb);
}

// bce: the section titles select their section (sectioned-scroll__title--active)
static void store_section(const HtmlElem root, const int section) {
    M.store_section = section;
    HtmlElem scrolls[4];
    const int n = html_dom_query_all(root, ".sectioned-scroll", scrolls, 4);
    for (int s = 0; s < n; s++) {
        HtmlElem titles[8];
        const int tn = html_dom_query_all(scrolls[s], ".sectioned-scroll__title", titles, 8);
        for (int i = 0; i < tn; i++) html_dom_set_class(titles[i], "sectioned-scroll__title--active", i == section);
    }
}

static void store_fill(const HtmlElem root) {
    fill_currency(root);
    HtmlElem titles[16];
    const int tn = html_dom_query_all(root, ".sectioned-scroll__title", titles, 16);
    for (int i = 0; i < tn; i++) {
        char hook[48];
        snprintf(hook, sizeof(hook), "strikeAction('store-section:%d')", i % 3);
        html_dom_set_attr(titles[i], "onclick", hook);
    }
    store_section(root, M.store_section);
    // the Spins section's Sr content: the wheels (OC)
    char *wheels = store_wheels_html();
    HtmlElem content[4];
    const int cn = html_dom_query_all(root, ".sectioned-scroll__section[data-index=\"0\"] .frvr-store-section__content", content, 4);
    for (int i = 0; i < cn && wheels; i++) html_dom_set_html(content[i], wheels);
    free(wheels);
}

// ---------------------------------------------------------------- SPIN (SA / aue / oU)

static void spin_fill(const HtmlElem root) {
    const StrikeWheel *w = &WHEELS[M.spin >= 0 && M.spin < WHEEL_COUNT ? M.spin : WHEEL_DEFAULT];
    char buf[64];
    set_text(root, ".spinner-title", w->name);
    // oU: one row per reward pool, weight * 100 %
    const HtmlElem odds = html_dom_query(root, ".odds");
    if (odds) {
        StrBuf sb = {0};
        for (int i = 0; i < WHEEL_ODD_COUNT; i++) {
            sb_appendf(&sb, "<div class=\"odd\"><div class=\"odd-icon\" style=\"background: %s;\"></div><div class=\"odd-name\">", WHEEL_ODDS[i].color);
            sb_append_esc(&sb, WHEEL_ODDS[i].name);
            sb_appendf(&sb, "</div><div class=\"odd-chance\">%g%%</div></div>", WHEEL_ODDS[i].weight * 100.0);
        }
        char *html = sb_finish(&sb);
        html_dom_set_html(odds, html);
        free(html);
    }
    // buttonTicketsSplit: 10x the price on top, 1x below
    snprintf(buf, sizeof(buf), "%d", w->amount * 10);
    set_text(root, "#spinTopSection .spin-tickets span", buf);
    snprintf(buf, sizeof(buf), "%d", w->amount);
    set_text(root, "#oneTicketSpin .spin-tickets span", buf);
    // the header's currencies (A1) only for a spin-ticket wheel
    const HtmlElem header = html_dom_query(root, ".frvr-controls-right .currency-header");
    if (header) {
        if (w->ticket) fill_currency(root);
        else html_dom_remove(header);
    }
    M.wheel_f = 0.0f;
}

// aue.resize + render: the canvas is F x F, F = the smaller side of its parent's content box (the
// parent is .spin-wheel__container, padded 74 * --base-scale on top and --screen-margin = 16 *
// --base-scale below, with the page's --base-scale when resize runs), the container's --base-scale
// then F / 280 px; the wheel (disc H = F / 2 * 0.95, slices at 0.95 H by weight from angle 0, the
// pointer, the centre disc 0.6 H) is spin_wheel.c's, drawn into this canvas.
static void spin_tick(void) {
    M.wheel.visible = 0;
    const HtmlElem root = strike_screen_root(STRIKE_SCREEN_SPIN);
    if (!root || strike_screen_top() != STRIKE_SCREEN_SPIN) return;
    const HtmlElem container = html_dom_query(root, ".spin-wheel__container");
    const HtmlElem canvas = container ? html_dom_query(container, "canvas") : 0;
    HtmlRect pr;
    if (!canvas || !html_dom_get_rect(container, &pr) || pr.w <= 0 || pr.h <= 0) return;
    float vw = 0.0f, vh = 0.0f;
    html_dom_viewport(&vw, &vh, NULL);
    // :root --base-scale: .2777777778vh, .15625vw under (max-aspect-ratio: 16/9)
    const float base = vh > 0.0f && vw / vh < 16.0f / 9.0f ? vw / 640.0f : vh / 360.0f;
    const float f = floorf(fminf(pr.w, pr.h - 90.0f * base));
    if (f <= 0.0f) return;
    if (fabsf(f - M.wheel_f) >= 1.0f) {
        M.wheel_f = f;
        char buf[32];
        snprintf(buf, sizeof(buf), "%dpx", (int) f);
        html_dom_set_style(canvas, "width", buf);
        html_dom_set_style(canvas, "height", buf);
        snprintf(buf, sizeof(buf), "%gpx", f / 280.0f);
        html_dom_set_style(container, "--base-scale", buf);
    }
    HtmlRect cr;
    if (!html_dom_get_rect(canvas, &cr) || cr.w <= 0 || cr.h <= 0) return;
    SpinWheelDraw *d = &M.wheel;
    memset(d, 0, sizeof(*d));
    d->visible = 1;
    d->canvas_elem = canvas;
    d->x = cr.x;
    d->y = cr.y;
    d->w = cr.w;
    d->h = cr.h;
    d->canvas_w = M.wheel_f;
    d->canvas_h = M.wheel_f;
    d->radius = M.wheel_f / 2.0f * 0.95f;
    float from = 0.0f;
    for (int i = 0; i < WHEEL_ODD_COUNT && i < SPIN_WHEEL_MAX_SLICES; i++) {
        const float span = (float) (2.0 * M_PI * WHEEL_ODDS[i].weight);
        d->slice_from[i] = from;
        d->slice_to[i] = from + span;
        unsigned r = 0, g = 0, b = 0;
        sscanf(WHEEL_ODDS[i].color + 1, "%02x%02x%02x", &r, &g, &b);
        d->slice_rgb[i][0] = (float) r / 255.0f;
        d->slice_rgb[i][1] = (float) g / 255.0f;
        d->slice_rgb[i][2] = (float) b / 255.0f;
        from += span;
        d->slices = i + 1;
    }
    d->pointer_count = 1; // p = [0]: one pointer, straight up
    d->pointer_rotation[0] = 0.0f;
    d->pointer_scale = 1.0f; // V = 1 before a spin
}

const SpinWheelDraw *strike_menu_wheel_draw(void) { return &M.wheel; }

// ---------------------------------------------------------------- CUSTOMIZE_CONTROLS (wb)

// a5.oncreate in Customize mode (and on resize): .control-buttons' --base-scale is
// `calc(${clientHeight} / 360 * 1px` (FRVR's string has no closing paren; a browser closes it at the end
// of the declaration, this engine does not, so the value here is the closed calc()). The capture's
// inline value (282 at its size) is replaced as soon as the box has a height.
static void customize_controls_tick(void) {
    const HtmlElem root = strike_screen_root(STRIKE_SCREEN_CUSTOMIZE_CONTROLS);
    const HtmlElem box = root ? html_dom_query(root, ".control-buttons--customize") : 0;
    if (!box) return;
    const int h = (int) html_dom_offset_height(box);
    char want[64], have[64] = "";
    snprintf(want, sizeof(want), "calc(%d / 360 * 1px)", h);
    html_dom_get_attr(box, "data-kn-scale", have, sizeof(have));
    if (h <= 0 || !strcmp(want, have)) return;
    html_dom_set_attr(box, "data-kn-scale", want);
    html_dom_set_style(box, "--base-scale", want);
    // the joysticks' nipplejs (static, rhe): size = --control-joystick-size at that scale (the capture's
    // 83 px = 106 * 282 / 360), the front half of it, both centred by negative margins
    HtmlElem sticks[4];
    const int n = html_dom_query_all(box, ".joystick", sticks, 4);
    for (int i = 0; i < n; i++) {
        char style[256] = "";
        html_dom_get_attr(sticks[i], "style", style, sizeof(style));
        const char *k = strstr(style, "--control-joystick-size:");
        if (!k) continue;
        const float size = (float) atof(k + strlen("--control-joystick-size:")) * (float) h / 360.0f;
        static const char *const PARTS[2] = {".back", ".front"};
        for (int p = 0; p < 2; p++) {
            const HtmlElem el = html_dom_query(sticks[i], PARTS[p]);
            if (!el) continue;
            const float px = p ? size / 2.0f : size;
            char v[32];
            snprintf(v, sizeof(v), "%gpx", px);
            html_dom_set_style(el, "width", v);
            html_dom_set_style(el, "height", v);
            snprintf(v, sizeof(v), "%gpx", -px / 2.0f);
            html_dom_set_style(el, "margin-left", v);
            html_dom_set_style(el, "margin-top", v);
        }
    }
}

static void customize_controls_fill(const HtmlElem root) {
    // the saved layout's positions, and the drag targets (strike_touch.c)
    strike_touch_customize_apply(html_dom_query(root, ".control-buttons--customize"));
    // rightContent: _t "Reset" (onclick Ahe()), then the home button
    HtmlElem buttons[4];
    const int n = html_dom_query_all(root, ".frvr-controls-right > .frvr-button", buttons, 4);
    for (int i = 0; i < n; i++) {
        char cls[160] = "";
        html_dom_get_attr(buttons[i], "class", cls, sizeof(cls));
        if (!strstr(cls, "frvr-button-home")) html_dom_set_attr(buttons[i], "onclick", "strikeAction('controls-reset')");
    }
}

// ---------------------------------------------------------------- LEADERBOARD_PLAYERS (YB / K5)

// YB's enemyPlayers: the tournament board's rows (A1e). Without hosted tournaments only a
// --render-scenario fixture supplies them; none = no rows.
static int board_rows(BoardRow *rows, const int max) {
    if (!M.board_fixture) return 0;
    const int n = M.board_count < max ? M.board_count : max;
    memcpy(rows, M.board, sizeof(BoardRow) * (size_t) n);
    return n;
}

// YB.view: the header row (f1e) stays, then one K5 row per player (stats: Score, Kills)
static void leaderboard_fill(const HtmlElem root) {
    const HtmlElem content = html_dom_query(root, ".frvr-lboard-end-screen__content");
    if (!content) return;
    HtmlElem old[BOARD_MAX + 8];
    const int on = html_dom_query_all(content, ".frvr-lboard-end-screen__row-wrapper", old, BOARD_MAX + 8);
    for (int i = 0; i < on; i++) {
        char cls[160] = "";
        html_dom_get_attr(old[i], "class", cls, sizeof(cls));
        if (!strstr(cls, "frvr-lboard-end-screen__header")) html_dom_remove(old[i]);
    }
    BoardRow rows[BOARD_MAX];
    const int n = board_rows(rows, BOARD_MAX);
    StrBuf sb = {0};
    for (int i = 0; i < n; i++) {
        const BoardRow *r = &rows[i];
        const char *P = "frvr-lboard-end-screen__";
        sb_appendf(&sb, "<div class=\"%srow-wrapper\" style=\"--row-index: %d;\"><div class=\"%srow\">", P, i + 1, P);
        sb_appendf(&sb, "<div class=\"%sposition %sposition--%s\"><span>%d</span></div>", P, P, r->you ? "player" : "ffa", i + 1);
        sb_appendf(&sb, "<div class=\"%spfp\"><img src=\"./cdn/textures/classes/icon_%d.png\" style=\"image-rendering: pixelated;\"></div>", P, r->class_id);
        sb_appendf(&sb, "<div class=\"%splayer-name%s\"><span><strong>", P, r->you ? " frvr-lboard-end-screen__player-name--player" : "");
        sb_append_esc(&sb, r->name);
        sb_appendf(&sb, "</strong></span></div><div class=\"%sstats\">", P);
        for (int s = 0; s < 2; s++) {
            const char *mod = r->you ? " frvr-lboard-end-screen__stat-col--player" : s % 2 == 0 ? "" : " frvr-lboard-end-screen__stat-col--odd";
            sb_appendf(&sb, "<div class=\"%sstat-col%s\"><span>", P, mod);
            if (r->has_stats) sb_appendf(&sb, "%d", s == 0 ? r->score : r->kills);
            sb_append(&sb, "</span></div>");
        }
        sb_append(&sb, "</div></div></div>");
    }
    char *html = sb_finish(&sb);
    if (html && *html) html_dom_insert_html(content, HTML_INSERT_BEFORE_END, html);
    free(html);
}

void strike_menu_scenario_board(const cJSON *rows) {
    if (!cJSON_IsArray(rows)) return;
    M.board_fixture = 1;
    M.board_count = 0;
    const cJSON *row;
    cJSON_ArrayForEach(row, rows) {
        if (M.board_count >= BOARD_MAX) break;
        BoardRow *r = &M.board[M.board_count++];
        memset(r, 0, sizeof(*r));
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(row, "name");
        snprintf(r->name, sizeof(r->name), "%s", cJSON_IsString(name) ? name->valuestring : "Guest");
        r->class_id = (int) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(row, "classIndex"));
        if (r->class_id < 0) r->class_id = 0;
        r->you = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(row, "you"));
        const cJSON *score = cJSON_GetObjectItemCaseSensitive(row, "score");
        const cJSON *kills = cJSON_GetObjectItemCaseSensitive(row, "kills");
        r->has_stats = cJSON_IsNumber(score) || cJSON_IsNumber(kills);
        r->score = cJSON_IsNumber(score) ? score->valueint : 0;
        r->kills = cJSON_IsNumber(kills) ? kills->valueint : 0;
    }
    const HtmlElem root = strike_screen_root(STRIKE_SCREEN_LEADERBOARD_PLAYERS);
    if (root) leaderboard_fill(root);
}

// ---------------------------------------------------------------- POPUP (In)

static void popup_fill(const HtmlElem root) {
    // isDismissableOnBackground (default true): the dark blur's onclick kt()
    const HtmlElem blur = html_dom_query(root, ".frvr-dark-blur-75");
    if (blur) html_dom_set_attr(blur, "onclick", "strikeAction('back')");
}

// ---------------------------------------------------------------- CLASS_SELECTION (B5 / ode / lde)

static void class_selection_fill(const HtmlElem root) {
    HtmlElem holders[STRIKE_CLASS_COUNT];
    const int n = html_dom_query_all(root, ".class-selection__class-bullet-holder", holders, STRIKE_CLASS_COUNT);
    for (int i = 0; i < n; i++) {
        const int sel = i == M.previewed;
        html_dom_set_class(holders[i], "class-selection__class-bullet-holder--selected", sel);
        const HtmlElem bullet = html_dom_query(holders[i], ".class-selection__class-bullet");
        if (bullet) html_dom_set_class(bullet, "class-selection__class-bullet--selected", sel);
        // the shine (only the selected bullet has it)
        const HtmlElem shine = bullet ? html_dom_query(bullet, ".class-selection__shine") : 0;
        if (shine && !sel) html_dom_remove(shine);
        else if (!shine && sel && bullet)
            html_dom_insert_html(bullet, HTML_INSERT_AFTER_BEGIN,
                                 "<div class=\"class-selection__shine\"><div class=\"shine-holder\" style=\"--max-width: 100%;\"><div class=\"shine\">"
                                 "<div class=\"shine__big\"></div><div class=\"shine__small\"></div></div></div></div>");
    }
    const StrikeClass *c = &CLASSES[M.previewed];
    char buf[96];
    const HtmlElem slot = html_dom_query(root, ".class-selection__selected-weapon-slot img");
    if (slot) html_dom_set_attr(slot, "src", c->slot_img);
    const HtmlElem rarity = html_dom_query(root, ".class-selection__selected-rarity");
    if (rarity) {
        snprintf(buf, sizeof(buf), "class-selection__selected-rarity class-rarity-banner--%d", c->rarity);
        html_dom_set_attr(rarity, "class", buf);
    }
    snprintf(buf, sizeof(buf), "\xE2\x80\xA2 %s", c->name);
    set_text(root, ".class-selection__selected-name span", buf);
    set_text(root, ".class-selection__selected-description span", c->description);
    set_text(root, ".class-selection__selected-weapon--name", c->weapon);
    // _t: disabled + "Selected" for the class in use, else "Select"
    const HtmlElem btn = html_dom_query(root, ".class-selection__select-class .frvr-button");
    if (btn) {
        const int in_use = M.previewed == current_class();
        if (in_use) html_dom_set_attr(btn, "disabled", "");
        else html_dom_remove_attr(btn, "disabled");
        html_dom_set_class(btn, "is-disabled", in_use);
        set_text(btn, ".frvr-button-text", in_use ? "Selected" : "Select");
    }
}

// ---------------------------------------------------------------- SETTINGS (Dy): values

// The rows of the Display / Game / Sound tabs by label (FRVR's settings list: settings.quality.*,
// settings.mobile.*, settings.audio.*; the key is its setting key, the default the capture's value
// on a fresh guest, tools/uidiff/scenarios/strike/settings-*.json). Values live in the shared
// settings store (ui_settings.c) so the audio sliders drive client_apply_setting like v10's.
typedef struct { const char *label, *key; double def; } StrikeSetting;
static const StrikeSetting SETTING_ROWS[] = {
    {"Resolution", "resolution", 0.4}, {"Frame Cap", "updateRate", 60}, {"Low Spec", "lowSpec", 0}, {"Particles", "particles", 1},
    {"Particle Distance", "particlesDist", 600}, {"Texture Animations", "textureAnim", 1}, {"Object Animations", "objectAnim", 1},
    {"Show Hit Indicators", "hitIndicators", 1}, {"Show Ping", "showPing", 0}, {"Show FPS", "showFPS", 0},
    {"Auto Fire", "autoFireMbl", 1}, {"Aim Assist", "aimAssist", 1}, {"X Sensitivity", "sensitivityXMbl", 1.4},
    {"Y Sensitivity", "sensitivityYMbl", 1.4}, {"Aim X Sensitivity", "aimSensitivityXMbl", 1.4}, {"Aim Y Sensitivity", "aimSensitivityYMbl", 1.4},
    {"Invert Y-Axis", "invertYMbl", 0}, {"Vibration", "vibrationMbl", 1}, {"Gradual Speed", "gradSpeedMbl", 0},
    {"Show jump button and disable tapping screen to jump", "mob_ctrl_disable_tap_jump", 0},
    // FOV / Weapon FOV: 1.37.2's `config.fov + 20` / `+ 15` (config.fov 50), the defaults ui_settings.c sets
    // under Strike (strike_rules.h settings, tools/strike/strike_data.json input.settings)
    {"Field of View (FOV)", "fov", 70}, {"Weapon FOV", "fpsFOV", 65}, {"Left Handed", "leftHanded", 0},
    {"Master Volume", "sound", 0.8}, {"Ambient Volume", "ambientVolume", 1}, {"Action Volume", "dialogueVolume", 1},
    {"Weapon Volume", "gunsVolume", 0.3}, {"Player Volume", "playerVolume", 1}, {"Cosmetics Volume", "skinVolume", 1},
    {"UI Volume", "uiVolume", 1}, {"Asset Volume", "assetVolume", 1},
};

static const StrikeSetting *setting_row(const char *label) {
    for (size_t i = 0; i < sizeof(SETTING_ROWS) / sizeof(SETTING_ROWS[0]); i++) {
        if (!strcmp(SETTING_ROWS[i].label, label)) return &SETTING_ROWS[i];
    }
    return NULL;
}

static double setting_number(const StrikeSetting *r) {
    double v = r->def;
    if (ui_setting_number(r->key, &v)) return v;
    const cJSON *saved = ui_setting_saved(r->key);
    if (cJSON_IsBool(saved)) return cJSON_IsTrue(saved);
    return r->def;
}

static void fmt_number(const double v, char *buf, const size_t n) {
    snprintf(buf, n, "%g", v);
}

// window.setSetting(key, value) from a slider / switch; the number box and the range follow
static int h_strike_setting(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user;
    if (argc < 1 || args[0].kind != HTML_ARG_STR || !args[0].str) return 1;
    const char *key = args[0].str;
    char buf[32];
    if (ev->target && !strcmp(ev->type, "click")) {
        snprintf(buf, sizeof(buf), "%s", html_dom_get_checked(ev->target) ? "true" : "false");
        ui_settings_set_string(key, buf);
        return 0;
    }
    ui_settings_set_string(key, ev->value ? ev->value : "0");
    char id[96];
    snprintf(id, sizeof(id), "slid_input_%s", key);
    const HtmlElem box = html_dom_by_id(id);
    if (box && box != ev->target) html_dom_set_value(box, ev->value ? ev->value : "0");
    snprintf(id, sizeof(id), "slid_%s", key);
    const HtmlElem range = html_dom_by_id(id);
    if (range && range != ev->target) html_dom_set_value(range, ev->value ? ev->value : "0");
    return 0;
}

// the rows of a tab's markup take their values from the store (FRVR: SETTINGS.list[i].val)
static void settings_values(const HtmlElem content) {
    HtmlElem rows[24];
    const int n = html_dom_query_all(content, ".setting", rows, 24);
    for (int i = 0; i < n; i++) {
        const HtmlElem name = html_dom_query(rows[i], ".settName");
        char label[96] = "";
        if (name) html_dom_get_text(name, label, sizeof(label));
        const StrikeSetting *r = setting_row(label);
        if (!r) continue;
        char buf[64], hook[160];
        const double v = setting_number(r);
        const HtmlElem range = html_dom_query(rows[i], "input[type=range]");
        const HtmlElem box = html_dom_query(rows[i], "input[type=number]");
        const HtmlElem check = html_dom_query(rows[i], "input[type=checkbox]");
        if (range) {
            fmt_number(v, buf, sizeof(buf));
            html_dom_set_value(range, buf);
            if (box) html_dom_set_value(box, buf);
            snprintf(hook, sizeof(hook), "strikeSetting('%s')", r->key);
            html_dom_set_attr(range, "oninput", hook);
            if (box) html_dom_set_attr(box, "oninput", hook);
        }
        if (check) {
            html_dom_set_checked(check, v != 0.0);
            snprintf(hook, sizeof(hook), "strikeSetting('%s')", r->key);
            html_dom_set_attr(check, "onclick", hook);
        }
    }
}

// ---------------------------------------------------------------- SETTINGS (Dy)

static void settings_tab(const HtmlElem root, const int tab) {
    M.settings_tab = tab;
    HtmlElem items[8];
    const int n = html_dom_query_all(root, ".tabSelectorItem", items, 8);
    for (int i = 0; i < n; i++) html_dom_set_class(items[i], "selected", i == tab);
    const HtmlElem content = html_dom_query(root, ".tabsContent");
    if (!content) return;
    char name[32];
    snprintf(name, sizeof(name), "SETTINGS-%s", SETTINGS_TABS[tab]);
    char *html = strike_template(name, NULL, 0);
    if (html) {
        html_dom_set_html(content, html);
        free(html);
    }
    settings_values(content);
    if (tab == 0) {
        HtmlElem presets[4];
        const int pn = html_dom_query_all(content, ".frvr-preset__button", presets, 4);
        for (int i = 0; i < pn; i++) html_dom_set_class(presets[i], "frvr-preset__button--active", i == M.preset);
    }
}

// ---------------------------------------------------------------- actions

static void play(void) {
    // quickConnect: Fm(Al, {info: "Loading <game> ..."}), then the game (RA when it starts)
    strike_loading_show("Loading  ...", 0);
    menu_play();
}

static void action(const char *a) {
    printf("[strike] action %s\n", a);
    if (strike_meta_action(a)) return; // missions tab, match details report, rewards track (strike_meta.c)
    if (!strcmp(a, "play")) play();
    else if (!strcmp(a, "back")) {
        strike_screen_pop(); // kt()
        if (strike_screen_top() == STRIKE_SCREEN_NONE && html_ui_is_menu_open()) strike_screen_push(STRIKE_SCREEN_MAIN_MENU);
    } else if (!strncmp(a, "pop:", 4)) {
        // a close button that is onHide() + kt(): SPIN's home from the store (kt() twice), Customize
        // Controls' home (its onHide kt() and its own: Settings goes too)
        for (int i = atoi(a + 4); i > 0; i--) strike_screen_pop();
        if (strike_screen_top() == STRIKE_SCREEN_NONE && html_ui_is_menu_open()) strike_screen_push(STRIKE_SCREEN_MAIN_MENU);
    } else if (!strncmp(a, "spin:", 5)) {
        const int i = atoi(a + 5);
        if (i >= 0 && i < WHEEL_COUNT) {
            M.spin = i;
            strike_screen_push(STRIKE_SCREEN_SPIN); // eb(id, {onHide: kt}) = Pt(SA, {spinnerIndex})
        }
    } else if (!strncmp(a, "store-section:", 14)) {
        const HtmlElem root = strike_screen_root(STRIKE_SCREEN_STORE);
        if (root) store_section(root, atoi(a + 14));
    } else if (!strcmp(a, "controls-reset")) {
        // Ahe(): mobile_layouts[current] = {} saved -> every control back at its Gw position
        printf("[strike] controls reset\n");
        strike_touch_layout_reset();
    } else if (!strcmp(a, "home")) {
        if (!html_ui_is_menu_open() && M.client) client_set_menu_open(M.client, 1);
        strike_screen_replace(STRIKE_SCREEN_MAIN_MENU); // nu(yg)
    } else if (!strcmp(a, "settings")) strike_screen_push(STRIKE_SCREEN_SETTINGS);
    else if (!strcmp(a, "customize")) strike_screen_push(STRIKE_SCREEN_CUSTOMIZATION);
    else if (!strcmp(a, "class-selection")) strike_screen_push(STRIKE_SCREEN_CLASS_SELECTION);
    else if (!strcmp(a, "profile")) strike_screen_push(STRIKE_SCREEN_PROFILE);
    else if (!strcmp(a, "store")) {
        M.store_section = 0; // Pt(cc()): the first section
        strike_screen_push(STRIKE_SCREEN_STORE);
    } else if (!strncmp(a, "buy:", 4)) {
        // a currency's "+" (nue): ly() (every screen above the first goes) then Pt(cc(), {section}).
        // FRVR's fg store (bce) ignores attrs.section and opens on Spins; its PI store (tce) selects
        // the section, as here.
        while (strike_screen_depth() > 1) strike_screen_pop();
        M.store_section = atoi(a + 4);
        if (strike_screen_shown(STRIKE_SCREEN_STORE)) {
            const HtmlElem root = strike_screen_root(STRIKE_SCREEN_STORE);
            if (root) store_section(root, M.store_section);
        } else {
            strike_screen_push(STRIKE_SCREEN_STORE);
        }
    }
    else if (!strcmp(a, "customize-controls")) strike_screen_push(STRIKE_SCREEN_CUSTOMIZE_CONTROLS); // Pt(ZC, {onHide})
    else if (!strcmp(a, "match-details")) strike_screen_push(STRIKE_SCREEN_MATCH_DETAILS);
    else if (!strncmp(a, "class:", 6)) {
        const int i = atoi(a + 6);
        if (i >= 0 && i < STRIKE_CLASS_COUNT) {
            M.previewed = i;
            const HtmlElem root = strike_screen_root(STRIKE_SCREEN_CLASS_SELECTION);
            if (root) class_selection_fill(root);
        }
    } else if (!strcmp(a, "select-class")) {
        if (M.previewed != current_class()) {
            menu_set_class_index(CLASSES[M.previewed].id, 0); // window.selectClass(_r)
            strike_screen_pop();                               // kt()
        }
    } else if (!strncmp(a, "settings-tab:", 13)) {
        const int t = atoi(a + 13);
        const HtmlElem root = strike_screen_root(STRIKE_SCREEN_SETTINGS);
        if (root && t >= 0 && t < (int) (sizeof(SETTINGS_TABS) / sizeof(SETTINGS_TABS[0]))) settings_tab(root, t);
    } else if (!strncmp(a, "preset:", 7)) {
        M.preset = atoi(a + 7);
        const HtmlElem root = strike_screen_root(STRIKE_SCREEN_SETTINGS);
        if (root) settings_tab(root, 0);
    }
}

static int h_strike_action(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user;
    (void) ev;
    if (argc < 1 || args[0].kind != HTML_ARG_STR || !args[0].str) return 1;
    action(args[0].str);
    html_ui_stop_propagation(); // one action per click (nested hooks: the bullet inside its holder)
    return 0;
}

void strike_menu_action(const char *name) { action(name); }

// ---------------------------------------------------------------- strike_ui.c hooks

const char *strike_menu_template(const StrikeScreen s) {
    switch (s) {
    case STRIKE_SCREEN_MAIN_MENU:
    case STRIKE_SCREEN_CLASS_SELECTION:
    case STRIKE_SCREEN_TAP_TO_PLAY:
    case STRIKE_SCREEN_SETTINGS:
    case STRIKE_SCREEN_CUSTOMIZATION:
    case STRIKE_SCREEN_BATTLEPASS:
    case STRIKE_SCREEN_CREATE_MATCH:
    case STRIKE_SCREEN_LEAN_END:
    case STRIKE_SCREEN_PROFILE:
    case STRIKE_SCREEN_STORE:
    case STRIKE_SCREEN_SPIN:
    case STRIKE_SCREEN_CUSTOMIZE_CONTROLS:
    case STRIKE_SCREEN_LEADERBOARD_PLAYERS:
    case STRIKE_SCREEN_POPUP:
    case STRIKE_SCREEN_MATCH_DETAILS:   // transcribed from the bundle (strike_meta.c)
    case STRIKE_SCREEN_REWARD_TO_CLAIM: // transcribed from the bundle (strike_meta.c)
        return strike_screen_id(s);
    default:
        return NULL; // no template of it yet (README.md "Not ported yet"); MISSION_TAB_SCREEN is
                     // the Battle Pass screen's Missions tab (strikeAction('missions'))
    }
}

void strike_menu_install(void) {
    html_ui_add_handler("strikeAction", h_strike_action);
    html_ui_add_handler("strikeSetting", h_strike_setting);
}

// FRVR draws the menu's character into the screen's own three.js canvas (MAIN_MENU, CLASS_SELECTION).
// Here that canvas is #classPreviewCanvas, the native class preview (client/src/ui/class_preview.c,
// menu_send_preview_rect in menu_ui.c). `box` (a selector, "canvas" or the holder that FRVR sizes)
// is placed in percent of the screen, and the canvas fills it.
static void class_preview_canvas(const HtmlElem root, const char *box, const float left, const float top, const float w, const float h) {
    const HtmlElem canvas = html_dom_query(root, "canvas");
    if (!canvas) return;
    html_dom_set_attr(canvas, "id", "classPreviewCanvas");
    html_dom_set_style(canvas, "width", "100%");
    html_dom_set_style(canvas, "height", "100%");
    const HtmlElem el = !strcmp(box, "canvas") ? canvas : html_dom_query(root, box);
    if (!el) return;
    char buf[32];
    html_dom_set_style(el, "position", "absolute");
    snprintf(buf, sizeof(buf), "%g%%", left);
    html_dom_set_style(el, "left", buf);
    snprintf(buf, sizeof(buf), "%g%%", top);
    html_dom_set_style(el, "top", buf);
    snprintf(buf, sizeof(buf), "%g%%", w);
    html_dom_set_style(el, "width", buf);
    snprintf(buf, sizeof(buf), "%g%%", h);
    html_dom_set_style(el, "height", buf);
    html_dom_set_style(el, "bottom", "auto");
}

void strike_menu_render(const StrikeScreen s, const HtmlElem root) {
    switch (s) {
    case STRIKE_SCREEN_MAIN_MENU:
        class_preview_canvas(root, "canvas", 0.0f, 0.0f, 100.0f, 100.0f); // MAIN_MENU: canvas{width:100%;height:100%} of the screen
        fill_account(root);
        break;
    case STRIKE_SCREEN_TAP_TO_PLAY:
        fill_account(root);
        break;
    case STRIKE_SCREEN_CLASS_SELECTION:
        M.previewed = current_class(); // _r = loadout.classIndex
        class_selection_fill(root);
        // FRVR's capture: the holder's canvas is the screen's size too (1280x720 at DPR 2), the figure
        // where MAIN_MENU has it
        class_preview_canvas(root, ".class-selection__character-holder", 0.0f, 0.0f, 100.0f, 100.0f);
        break;
    case STRIKE_SCREEN_SETTINGS:
        if (M.settings_tab != 0) settings_tab(root, M.settings_tab);
        else {
            HtmlElem presets[4];
            const int pn = html_dom_query_all(root, ".frvr-preset__button", presets, 4);
            for (int i = 0; i < pn; i++) html_dom_set_class(presets[i], "frvr-preset__button--active", i == M.preset);
        }
        break;
    case STRIKE_SCREEN_CUSTOMIZATION: {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s", CLASSES[current_class()].name);
        set_text(root, ".customization-screen__class-selection .frvr-button-text", buf);
        break;
    }
    case STRIKE_SCREEN_STORE:
        store_fill(root);
        break;
    case STRIKE_SCREEN_SPIN: {
        spin_fill(root);
        const HtmlElem home = html_dom_query(root, ".frvr-button-home");
        if (home) html_dom_set_attr(home, "onclick", "strikeAction('pop:2')"); // onClose: kt() + the store's onHide kt()
        break;
    }
    case STRIKE_SCREEN_CUSTOMIZE_CONTROLS: {
        customize_controls_fill(root);
        const HtmlElem home = html_dom_query(root, ".frvr-button-home");
        if (home) html_dom_set_attr(home, "onclick", "strikeAction('pop:2')"); // onClose: onHide() (kt) + kt()
        break;
    }
    case STRIKE_SCREEN_LEADERBOARD_PLAYERS:
        leaderboard_fill(root);
        break;
    case STRIKE_SCREEN_POPUP:
        popup_fill(root);
        break;
    default:
        break;
    }
    strike_meta_render(s, root); // the Battle Pass tabs, the Rewards Track, match details, reward card
}

int strike_menu_level(void) {
    char name[64];
    int level = 1, kr = 0;
    account_values(name, sizeof(name), &level, &kr);
    return level;
}

void strike_menu_tick(Client *client) {
    M.client = client;
    strike_meta_tick(client);
    spin_tick();
    customize_controls_tick();
    // the account box follows the account / name (Mithril redraws on every change)
    char name[64], key[160];
    int level = 1, kr = 0;
    account_values(name, sizeof(name), &level, &kr);
    snprintf(key, sizeof(key), "%s/%d/%d/%d", name, level, kr, client ? client->menu_class_index : 0);
    if (strcmp(key, M.last_account)) {
        snprintf(M.last_account, sizeof(M.last_account), "%s", key);
        const HtmlElem a = strike_screen_root(STRIKE_SCREEN_MAIN_MENU);
        if (a) fill_account(a);
        const HtmlElem b = strike_screen_root(STRIKE_SCREEN_TAP_TO_PLAY);
        if (b) fill_account(b);
    }
}
