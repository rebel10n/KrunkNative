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
//   UI POPUP_WARNING      the connection popups (Pt(UI, {message, actionText}): Lost Connection, Game
//                         Updating), strike_connect.c; its Reconnect button is strikeAction('reconnect')
//   YB LEADERBOARD_PLAYERS FRVR hosts it only in its tournament rewards popup (qB, In); KrunkNative has no
//                         hosted tournaments, so only a scenario (state.strike.leaderboard) shows it
// The Strike classes are FRVR's six (tools/strike/strike_data.json "classes"): Krunker class ids
// 0 Triggerman, 1 Hunter, 2 Run N Gun, 4 Shotgunner (Krunker's Vince), 5 Detective, 7 Rocketeer.
#include <strike_ui.h>
#include <player_names.h> // Hide player names (player_names.h)
#include <strike_notify.h>
#include <strike_ftue.h>
#include <html_ui.h>
#include <ui_logic.h>
#include <backend_ui.h>
#include <spin_wheel.h>
#include <strike_double_xp.h>
#include <strike_hud_fx.h> // the Damage Number Display's modes and labels (its dropdown)
#include <render_settings.h> // the V-Sync row and the Fullscreen Mode button (ui_render_settings.c)
#include <i18n.h>            // the rows' kn.* copy (I18N)
#include <strike_econ.h> // the account's Strike wallet, items, spins, shop, leaderboards (kn_brain /strike)
#include <ruleset.h>     // rules_classes(): the class's primary weapon (its skin slot)
#include <audio.h>       // SOUND.play("reward") when the wheel stops
#include <client.h>
#include <net_ext.h> // NET_EXT_KN_SWAP_TEAM (Switch Teams)
#include <gltf.h>    // gltf_clock(): the menu's clock for the Switch Teams count
#include <strike_banner_ad.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

// window.selectSettingPre's i (the bundle's jue): 0 Default, 1 Pro, 2 Custom (the start, selectedPresetIndex
// || 2), 3 Performance. The page's buttons come in the order Default, Pro, Performance, Custom
// (SETTINGS-presets.html: preset:0..3), so BUTTON_PRESET maps a button to its id.
enum { PRESET_DEFAULT = 0, PRESET_PRO = 1, PRESET_CUSTOM = 2, PRESET_PERFORMANCE = 3 };
static const int BUTTON_PRESET[4] = {PRESET_DEFAULT, PRESET_PRO, PRESET_PERFORMANCE, PRESET_CUSTOM};

// The shop's prize wheels (OC / SA): kn_brain's Strike catalogue (strike_economy()->wheels, the same
// rows /strike/v2/spin-wheels/list serves): the ones a shop lists (not hideInShop) with a price in KR or
// spin tickets. FRVR also lists its ad wheels (timeBetweenSpins: Gn.canShowAds) and the golden wheel
// only through the golden ticket; KrunkNative shows no ads, so neither is a block here.
// A --render-scenario with state.strike.account (the capture's guest) shows the capture sandbox's
// wheels instead (tools/uidiff/capture-strike.js SPIN_WHEELS), so its pixels compare with the reference.
typedef struct { char pool[24], name[32], color[16]; double weight; } OddView;
typedef struct {
    char id[32], name[48];
    int ticket; // price.currency spin_ticket (else kr)
    int amount; // price.amount
    OddView odds[SPIN_WHEEL_MAX_SLICES];
    int odd_count;
} WheelView;
#define WHEELS_MAX 8
static const struct { const char *id, *name; int ticket, amount; } CAPTURE_WHEELS[] = {
    {"lucky-draw", "Lucky Draw", 0, 500}, {"wheel-kr", "KR Spin", 0, 250}, {"wheel-ticket", "Ticket Spin", 1, 1},
};
static const OddView CAPTURE_ODDS[] = {
    {"common", "Common", "#a7b1c2", 0.5}, {"uncommon", "Uncommon", "#79d94a", 0.28}, {"rare", "Rare", "#3aa0ff", 0.14},
    {"epic", "Epic", "#b45cff", 0.06}, {"legendary", "Legendary", "#ffb800", 0.02},
};
// OC's grid spans per wheel count ("column/row")
static const char *const WHEEL_GRID[][8] = {
    {0}, {"12/2"}, {"6/2", "6/2"}, {"6/2", "6/1", "6/1"}, {"6/1", "6/1", "6/1", "6/1"},
};

#define BOARD_MAX 16
typedef struct {
    char name[32];
    int class_id, you;
    int has_stats, score, kills; // the two stat columns (Score / Kills, or a board's Level / value)
    int score_rank;              // the board's rank (0: the row's place)
} BoardRow;

typedef enum { POPUP_NONE, POPUP_LEADERBOARD, POPUP_PRODUCT, POPUP_BUY_TICKETS } PopupKind;

static struct {
    Client *client;
    int previewed;        // _r: the class bullet shown in CLASS_SELECTION
    int settings_tab;
    int preset;           // selectedPresetIndex: a PRESET_* id (FRVR's i, not the button's order)
    char last_account[160];
    WheelView wheels[WHEELS_MAX];
    WheelView golden;     // the golden wheel of the shop's front (golden_wheel)
    int wheel_count, wheels_fixture; // wheels_fixture: built for a scenario (-1: not built)
    int spin;             // SA's spinnerIndex (a wheels[] index)
    cJSON *shop;          // GET /strike/v1/shop (strike_econ_get_shop)
    // T5: the Customization category open in ITEM_SELECTION_SCREEN
    int item_cat;
    char item_default_img[256];
    char item_ids[64][24]; // the shown squares' item ids ("" = Default)
    int item_count;
    PopupKind popup;      // what POPUP's builder shows
    int ticket_n;         // Mf: the buy-tickets stepper (yU), kept across opens (0: 1)
    int ticket_busy;      // a buy-tickets purchase is on its way
    BoardRow live_board[BOARD_MAX];
    int live_board_count;
    char board_cols[2][16];
    char buying[64];      // a purchase on its way
    int product;          // U2: the M.shop product index the product popup shows
    // aue's spin (onSpin / render): p[] pointers -> d[] targets at speeds A[], then the won pool grows
    struct {
        int phase;        // 0 idle, 1 asked, 2 pointers turning, 3 the end (u -> f), 4 the grown pool held
        cJSON *rewards;   // b: the spin's rewards (results[].reward) for the reward screen (owned)
        int heroic;       // x: heroicSpinsTriggered
        int n;
        double p[SPIN_WHEEL_MAX_POINTERS], d[SPIN_WHEEL_MAX_POINTERS], A[SPIN_WHEEL_MAX_POINTERS];
        char pool[24];    // C: the won pool (rewards[0].poolId)
        double c, u, f;   // c poolWeight, u -> f = 100 - c * 100
        double step_ms, last_s;
        int wheel;        // the wheels[] index spun
    } sp;
    int store_section;    // bce: the active section title
    SpinWheelDraw wheel;  // SPIN's canvas this frame (spin_wheel.c draws it into the page's canvas)
    float wheel_f;        // aue's F (the canvas side, CSS px) last applied
    // state.strike.leaderboard of a --render-scenario (the capture's rows), used when set
    int board_fixture;
    BoardRow board[BOARD_MAX];
    int board_count;
    int swap_phase;       // Switch Teams: SWAP_IDLE / SWAP_IN (5 s) / SWAP_AGAIN (10 s)
    double swap_t0;       // gltf_clock() when the current count started
    char swap_shown[64];  // the button's label and state as last written (switch_teams_update)
    struct {
        int open;  // M5's kr-limit speech bubble is up (eM)
        int timer; // its 5 s hide timer (html_timer_add), 0 = none
    } kr_bubble;
} M = {.preset = PRESET_CUSTOM, .wheels_fixture = -1, .spin = -1};

static void popup_open(PopupKind kind);

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

// The signed-in account's Strike state (strike_econ.c: /strike/v1/players/me), 0 = none / not loaded
static const StrikeAccount *econ(void) {
    const StrikeAccount *a = strike_econ_account();
    return a->account_id > 0 && a->loaded ? a : NULL;
}

static void account_values(char *name, size_t nn, int *level, int *kr) {
    const AccountInfo *acc = active_account();
    const Client *c = M.client;
    const StrikeAccount *e = econ();
    if (FIXTURE.set) {
        snprintf(name, nn, "%s", FIXTURE.name);
        *level = FIXTURE.level;
        *kr = FIXTURE.kr;
    } else if (acc) {
        // jU: the account's name, its Strike level (the bundle's XP curve) and wallet.kr
        snprintf(name, nn, "%s", player_names_account(acc->alias[0] ? acc->alias : acc->name));
        *level = e ? e->level.level : acc->level > 0 ? acc->level : 1;
        *kr = e ? (int) e->wallet[STRIKE_CUR_KR] : 0;
    } else {
        snprintf(name, nn, "%s", c && c->menu_name[0] ? c->menu_name : "Guest");
        *level = 1;
        *kr = 0;
    }
}

// Po(Se.account): wallet.spin_ticket
static int spin_tickets(void) {
    if (FIXTURE.set) return FIXTURE.tickets;
    const StrikeAccount *e = econ();
    return e ? (int) e->wallet[STRIKE_CUR_SPIN_TICKET] : 0;
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
    if (n >= 1) html_dom_set_text(cur[0], (snprintf(buf, sizeof(buf), "%d", spin_tickets()), buf));
    const StrikeAccount *e = econ();
    if (FIXTURE.set || e) { // xpInCurrentLevel / xpToNextLevel
        snprintf(buf, sizeof(buf), "%lld", FIXTURE.set ? (long long) FIXTURE.xp : (long long) e->level.xp_in_level);
        set_text(root, ".account-current-xp", buf);
        snprintf(buf, sizeof(buf), "/%lld", FIXTURE.set ? (long long) FIXTURE.next_xp : (long long) e->level.xp_to_next);
        set_text(root, ".account-xp-to-next-level", buf);
    }
    // the face: the selected class's icon (Se.account face / getClassIcon)
    const HtmlElem img = html_dom_query(root, "#mobMenuAccImg img");
    if (img) {
        snprintf(buf, sizeof(buf), "./cdn/textures/classes/icon_%d.png", CLASSES[current_class()].id);
        html_dom_set_attr(img, "src", buf);
    }
}

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

static int wheel_cmp(const void *pa, const void *pb) {
    // OC's sort: a spin-ticket wheel after a KR one, else the higher price first
    const WheelView *a = pa, *b = pb;
    if (a->ticket != b->ticket) return a->ticket ? 1 : -1;
    return b->amount - a->amount;
}

static int odd_cmp(const void *pa, const void *pb) {
    const OddView *a = pa, *b = pb; // rewards.sort((a, b) => b.weight - a.weight)
    return a->weight < b->weight ? 1 : a->weight > b->weight ? -1 : 0;
}

// one WheelView from a catalogue wheel: its price, its odds (the pools' colours, sorted by weight)
static void wheel_view_fill(const StrikeWheel *sw, WheelView *w) {
    memset(w, 0, sizeof(*w));
    snprintf(w->id, sizeof(w->id), "%s", sw->id);
    snprintf(w->name, sizeof(w->name), "%s", sw->name);
    w->ticket = !strcmp(sw->price_currency, "spin_ticket");
    w->amount = sw->price_amount;
    for (int k = 0; k < sw->slice_count && w->odd_count < SPIN_WHEEL_MAX_SLICES; k++) {
        const StrikePool *pool = strike_pool(sw->slices[k].pool);
        OddView *o = &w->odds[w->odd_count++];
        snprintf(o->pool, sizeof(o->pool), "%s", sw->slices[k].pool);
        snprintf(o->name, sizeof(o->name), "%s", pool ? pool->name : sw->slices[k].pool);
        snprintf(o->color, sizeof(o->color), "%s", pool && pool->color ? pool->color : "#ffffff");
        o->weight = sw->slices[k].weight;
    }
    qsort(w->odds, (size_t) w->odd_count, sizeof(OddView), odd_cmp);
}

static void wheels_build(void) {
    const int fixture = FIXTURE.set;
    if (M.wheels_fixture == fixture) return;
    M.wheels_fixture = fixture;
    M.wheel_count = 0;
    if (fixture) {
        for (size_t i = 0; i < sizeof(CAPTURE_WHEELS) / sizeof(CAPTURE_WHEELS[0]); i++) {
            WheelView *w = &M.wheels[M.wheel_count++];
            memset(w, 0, sizeof(*w));
            snprintf(w->id, sizeof(w->id), "%s", CAPTURE_WHEELS[i].id);
            snprintf(w->name, sizeof(w->name), "%s", CAPTURE_WHEELS[i].name);
            w->ticket = CAPTURE_WHEELS[i].ticket;
            w->amount = CAPTURE_WHEELS[i].amount;
            w->odd_count = (int) (sizeof(CAPTURE_ODDS) / sizeof(CAPTURE_ODDS[0]));
            memcpy(w->odds, CAPTURE_ODDS, sizeof(CAPTURE_ODDS));
        }
    } else {
        const StrikeEconomy *e = strike_economy();
        for (int i = 0; i < e->wheel_count && M.wheel_count < WHEELS_MAX; i++) {
            const StrikeWheel *sw = &e->wheels[i];
            if (sw->hide_in_shop || !sw->price_currency) continue; // the golden wheel: golden_wheel(), from the front
            const int ticket = !strcmp(sw->price_currency, "spin_ticket");
            if (!ticket && strcmp(sw->price_currency, "kr") != 0) continue;
            wheel_view_fill(sw, &M.wheels[M.wheel_count++]);
        }
        qsort(M.wheels, (size_t) M.wheel_count, sizeof(WheelView), wheel_cmp);
    }
    // SA's spinnerIndex default: the capture's ticket wheel, else the first
    M.spin = 0;
    for (int i = 0; i < M.wheel_count; i++) {
        if (M.wheels[i].ticket) M.spin = i;
    }
}

// The Lucky Draw's golden spin (FRVR's lucky-draw component): the shop's front.sections SpinWheel
// content (kn_brain front_json), whose spinWheels name the golden_spin wheel. It is not in the Spins
// list (hideInShop) and has no M.wheels row: M.spin == GOLDEN_SPIN selects it.
#define GOLDEN_SPIN (-2)
static const cJSON *golden_front(void) {
    const cJSON *front = M.shop ? cJSON_GetObjectItemCaseSensitive(M.shop, "front") : NULL;
    const cJSON *sec;
    cJSON_ArrayForEach(sec, cJSON_GetObjectItemCaseSensitive(front, "sections")) {
        const cJSON *c;
        cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(sec, "content")) {
            const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(c, "type"));
            if (type && !strcmp(type, "spin_wheel")) return c;
        }
    }
    return NULL;
}

static const WheelView *golden_wheel(void) {
    const cJSON *c = golden_front();
    const cJSON *ref;
    cJSON_ArrayForEach(ref, cJSON_GetObjectItemCaseSensitive(c, "spinWheels")) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(ref, "spinWheelId"));
        const StrikeWheel *sw = id ? strike_wheel(id) : NULL;
        if (sw && sw->price_currency && !strcmp(sw->price_currency, "spin_golden_ticket")) {
            wheel_view_fill(sw, &M.golden);
            return &M.golden;
        }
    }
    return NULL;
}

static const WheelView *current_wheel(void) {
    if (M.spin == GOLDEN_SPIN) return golden_wheel();
    wheels_build();
    return M.wheel_count ? &M.wheels[M.spin >= 0 && M.spin < M.wheel_count ? M.spin : 0] : NULL;
}

// OC.view: one _I block per wheel (renderBackgroundImage: the wheel's imageUrl, none here), title r(p)
// (KR: the coin + amount, tickets: qf), subtitle the wheel's name, overlay l(p) (a ticket wheel: wce,
// the tickets owned), the click eb(p._id) -> SPIN
static char *store_wheels_html(void) {
    wheels_build();
    StrBuf sb = {0};
    const int count = M.wheel_count;
    for (int i = 0; i < count; i++) {
        const WheelView *w = &M.wheels[i];
        char col[8] = "", row[8] = "";
        const char *span = count < (int) (sizeof(WHEEL_GRID) / sizeof(WHEEL_GRID[0])) ? WHEEL_GRID[count][i] : "3/1";
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

// ---- the Market / Currency sections: GET /strike/v1/shop (strike_econ_get_shop). FRVR's fg builds
// Market from its N2 market and Currency from yU (ticket packs) + the KR packs (_I blocks,
// .store-krunkies); kn_brain's shop prices a product in KR or free, and a KR package in USD (shown as
// $ with no payment provider; the purchase credits it). Every product is an _I block
// here: title its price, subtitle its title, the click buys it (strike_econ_buy; FRVR first shows the
// product popup U2). Market = categories deals / bundles / free, Currency = tickets / currency.
static const char *product_section(const char *category) {
    if (!category) return NULL;
    if (!strcmp(category, "deals") || !strcmp(category, "bundles") || !strcmp(category, "free")) return "1";
    if (!strcmp(category, "tickets") || !strcmp(category, "currency")) return "2";
    return NULL;
}

// a product reward's item id: "item": "<id>" (the catalogue's), or kn_brain's resolved item object (its _id,
// the same two forms strike_reward.c reads); a picture needs the id, so the object form must parse too
static const char *reward_item_id(const cJSON *reward) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(reward, "item");
    if (cJSON_IsObject(item)) item = cJSON_GetObjectItemCaseSensitive(item, "_id");
    return cJSON_GetStringValue(item);
}

static void product_image(const cJSON *product, char *out, const size_t n) {
    out[0] = 0;
    const cJSON *reward = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(product, "products"), 0);
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reward, "type"));
    if (type && !strcmp(type, "item")) {
        const StrikeItem *it = strike_item(reward_item_id(reward));
        if (it && it->skin >= 0) store_skin_preview(it->skin, out, n);
    } else if (type && !strcmp(type, "currency")) {
        const char *cur = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reward, "currency"));
        const int amount = (int) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(reward, "amount"));
        if (cur && !strcmp(cur, "spin_ticket")) { // pa(): spin-ticket-1..3 / -many
            if (amount >= 1 && amount <= 3) snprintf(out, n, "./img/frvr-ui/spin-ticket-%d.png", amount);
            else snprintf(out, n, "./img/frvr-ui/spin-ticket-many.png");
        } else if (cur && !strcmp(cur, "kr")) {
            snprintf(out, n, "./img/currencies/krunkies.png");
        }
    }
}

static void store_products_fill(const HtmlElem root) {
    const cJSON *products = cJSON_GetObjectItemCaseSensitive(M.shop, "products");
    for (int section = 1; section <= 2; section++) {
        char want[4];
        snprintf(want, sizeof(want), "%d", section);
        StrBuf sb = {0};
        int index = 0, count = 0;
        const cJSON *p;
        cJSON_ArrayForEach(p, products) {
            const int i = index++;
            const char *sec = product_section(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(p, "category")));
            if (!sec || strcmp(sec, want) != 0) continue;
            const cJSON *price = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(p, "price"), 0);
            const char *ptype = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(price, "type"));
            const int amount = (int) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(price, "amount"));
            const int bought = (int) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(p, "purchaseCount"));
            const int max = (int) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(p, "maxPurchaseCount"));
            const int sold_out = max > 0 && bought >= max;
            char image[512];
            product_image(p, image, sizeof(image));
            sb_appendf(&sb, "<div class=\"frvr-store-block-container store-product%s\" style=\"grid-column: span 3;\" data-product=\"",
                       sold_out ? " is-gray is-locked" : "");
            sb_append_esc(&sb, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(p, "_id")) ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(p, "_id")) : "");
            sb_appendf(&sb, "\" onclick=\"strikeAction('buy-product:%d')\"><div class=\"frvr-store-block\">", i);
            if (image[0]) {
                sb_append(&sb, "<div class=\"frvr-store-block-render\" style=\"background-image: url('");
                sb_append_esc(&sb, image);
                sb_append(&sb, "');\"></div>");
            }
            if (sold_out) sb_append(&sb, "<span class=\"material-icons\">lock</span>");
            sb_append(&sb, "<div class=\"frvr-store-block-title\">");
            if (ptype && !strcmp(ptype, "free")) sb_append(&sb, "Free");
            else if (ptype && !strcmp(ptype, "real_money")) sb_appendf(&sb, "$%d.%02d", amount / 100, amount % 100); // a KR package: US cents
            else sb_appendf(&sb, "<img class=\"frvr-store-block-title__KRimg\" src=\"img/frvr-ui/icon-krunkie-coin.svg\"><span>%d</span>", amount);
            sb_append(&sb, "</div><div class=\"frvr-store-block-subtitle\">");
            sb_append_esc(&sb, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(p, "title")) ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(p, "title")) : "");
            sb_append(&sb, "</div></div></div>");
            count++;
        }
        char *blocks = sb_finish(&sb);
        char sel[96];
        snprintf(sel, sizeof(sel), ".sectioned-scroll__section[data-index=\"%d\"]", section);
        HtmlElem secs[4];
        const int n = html_dom_query_all(root, sel, secs, 4);
        for (int k = 0; k < n; k++) {
            // Sr: .frvr-store-section > .frvr-store-section__content
            StrBuf w = {0};
            if (count) {
                sb_append(&w, "<div class=\"frvr-store-section \"><div class=\"frvr-store-section__content\">");
                sb_append(&w, blocks ? blocks : "");
                sb_append(&w, "</div></div>");
            }
            char *html = sb_finish(&w);
            html_dom_set_html(secs[k], html ? html : "");
            free(html);
        }
        free(blocks);
    }
}

static void store_spins_fill(const HtmlElem root);

static void on_shop(void *user, const int status, const cJSON *data, const cJSON *error) {
    (void) user;
    (void) error;
    if (status != 200 || !cJSON_IsObject(data)) return;
    cJSON_Delete(M.shop);
    M.shop = cJSON_Duplicate(data, 1);
    printf("[strike] shop: %d products\n", cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(M.shop, "products")));
    const HtmlElem root = strike_screen_root(STRIKE_SCREEN_STORE);
    if (root) {
        store_products_fill(root);
        store_spins_fill(root); // the golden block follows the shop's front
    }
}

// $r's answer: ws goes (Tn() == ws && kt()), then Vw: Pt(Es, {rewards: products, origin: store})
// The purchase's answer has a deadline: a buy with no answer (the socket dropped with it) clears the blocker, so the
// store is not held behind "Purchasing..." for good (a late answer still shows what was bought)
#define PURCHASE_WAIT_MS 15000
static int g_purchase_timer;

// A purchase that did not go through (no answer in time, or refused) says so with Retry, which buys it again
static int g_buy_index = -1; // the product of the purchase on its way (purchase_retry runs it again)
static void purchase(int index);
static void purchase_retry(void) {
    if (g_buy_index >= 0) purchase(g_buy_index);
}

static void purchase_failed(void) {
    strike_popup_retry(I18N("kn.strike.purchaseFailed", "Couldn't complete the purchase. Try again."), purchase_retry);
}

static void purchase_expired(void *user) {
    (void) user;
    g_purchase_timer = 0;
    if (!M.buying[0]) return;
    printf("[strike] purchase %s: no answer in %d s\n", M.buying, PURCHASE_WAIT_MS / 1000);
    M.buying[0] = 0;
    strike_reward_blocker_hide();
    purchase_failed();
}

static void on_bought(void *user, const int status, const cJSON *data, const cJSON *error) {
    (void) user;
    const char *msg = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(error, "message"));
    html_timer_cancel(g_purchase_timer);
    g_purchase_timer = 0;
    strike_reward_blocker_hide();
    // a late answer after purchase_expired (M.buying cleared) is logged, not shown again
    const int waiting = M.buying[0] != 0;
    if (status == 200) printf("[strike] bought %s\n", M.buying);
    else printf("[strike] purchase refused (%d): %s\n", status, msg ? msg : "no answer");
    M.buying[0] = 0;
    strike_econ_get_shop(on_shop, NULL); // purchaseCount moved
    if (status == 200) strike_reward_show(cJSON_GetObjectItemCaseSensitive(data, "rewards"), "store", 0, NULL, NULL);
    else if (waiting) purchase_failed();
}

// $r(product, price, 1): Pt(ws, {loadingText: windows.store.purchasing}) while the shop answers
static void purchase(const int index) {
    const cJSON *p = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(M.shop, "products"), index);
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(p, "_id"));
    if (!id || M.buying[0]) return;
    snprintf(M.buying, sizeof(M.buying), "%s", id);
    g_buy_index = index;
    if (!strike_econ_buy(id, 1, on_bought, NULL)) {
        printf("[strike] purchase %s: no Strike account (sign in)\n", id); // $r: Pt(Zu) (no Strike login screen here)
        M.buying[0] = 0;
        purchase_failed(); // not sent: the error says so, Retry sends it again
        return;
    }
    strike_reward_blocker("Purchasing...");
    html_timer_cancel(g_purchase_timer);
    g_purchase_timer = html_timer_add(PURCHASE_WAIT_MS, purchase_expired, NULL);
}

// a product block's click: sU opens the product popup U2 for a paid item (Pt(In, {builder: U2})), a free
// or currency product is bought at once
static void buy_product(const int index) {
    const cJSON *p = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(M.shop, "products"), index);
    if (!p || M.buying[0]) return;
    char *popup = strike_product_popup_html(p, NULL);
    if (popup) {
        free(popup);
        M.product = index;
        popup_open(POPUP_PRODUCT);
        return;
    }
    purchase(index);
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
        // the title's onclick: .sectioned-scroll__content scrollBy(top: section top - content top)
        const HtmlElem content = html_dom_query(scrolls[s], ".sectioned-scroll__content");
        char sel[64];
        snprintf(sel, sizeof(sel), ".sectioned-scroll__section[data-index=\"%d\"]", section);
        const HtmlElem target = content ? html_dom_query(content, sel) : 0;
        HtmlRect cr, tr;
        HtmlScroll sc;
        if (target && html_dom_get_rect(content, &cr) && html_dom_get_rect(target, &tr) && html_dom_get_scroll(content, &sc))
            html_dom_set_scroll(content, sc.left, sc.top + (tr.y - cr.y));
    }
}

// the golden spin meter (FRVR's "Golden Spin Meter" bar, X/10): the account's count of paid spins
static char *golden_meter_html(void) {
    const StrikeAccount *a = strike_econ_account();
    const int need = a->spins_to_golden > 0 ? a->spins_to_golden : strike_economy()->golden_spin_meter;
    int have = a->golden_spin_count;
    if (have > need) have = need;
    if (have < 0) have = 0;
    StrBuf sb = {0};
    sb_appendf(&sb, "<div class=\"golden-spin-meter\"><div class=\"golden-spin-meter__head\">"
                    "<span class=\"golden-spin-meter__label\">Golden Spin Meter</span>"
                    "<span class=\"golden-spin-meter__value\">%d/%d</span></div>"
                    "<div class=\"golden-spin-meter__bar\"><div class=\"golden-spin-meter__fill\" style=\"width: %d%%;\"></div></div></div>",
               have, need, need > 0 ? have * 100 / need : 0);
    return sb_finish(&sb);
}

// the Lucky Draw's golden block (FRVR lce's golden meter and golden button) from the shop's front: the
// button spins the golden wheel when the account holds a golden ticket, else it buys one (the front's
// golden product, buy-product by its index in the shop's products)
static char *store_golden_html(void) {
    const cJSON *front = golden_front();
    if (FIXTURE.set || !front || !golden_wheel()) return NULL;
    const StrikeAccount *a = strike_econ_account();
    const int64_t tickets = a->wallet[STRIKE_CUR_GOLDEN_TICKET];
    const cJSON *ref = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(front, "products"), 0);
    const char *pid = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(ref, "productRef"), "productId"));
    int pindex = -1, price = 0;
    const cJSON *products = cJSON_GetObjectItemCaseSensitive(M.shop, "products");
    const cJSON *p;
    int i = 0;
    cJSON_ArrayForEach(p, products) {
        if (pid && !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(p, "_id")) ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(p, "_id")) : "", pid)) {
            pindex = i;
            price = (int) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(p, "price"), 0), "amount"));
        }
        i++;
    }
    char *meter = golden_meter_html();
    StrBuf sb = {0};
    sb_appendf(&sb, "<div id=\"luckyDrawGolden\" class=\"golden-spin-block\" style=\"grid-column: 1 / -1;\">%s<div class=\"golden-spin-block__actions\">", meter);
    if (tickets >= 1) {
        sb_appendf(&sb, "<div id=\"luckyDrawOneSpin\" class=\"golden-spin-block__button\" onclick=\"strikeAction('spin-golden')\">Spin 1 x</div>"
                        "<div class=\"golden-spin-block__tickets\">%lld golden tickets</div>", (long long) tickets);
    } else if (pindex >= 0) {
        sb_appendf(&sb, "<div id=\"luckyDrawOneSpin\" class=\"golden-spin-block__button\" onclick=\"strikeAction('buy-product:%d')\">Golden Spin Ticket %d KR</div>"
                        "<div class=\"golden-spin-block__tickets\">0 golden tickets</div>", pindex, price);
    }
    sb_append(&sb, "</div></div>");
    free(meter);
    return sb_finish(&sb);
}

// the Spins section's content (Sr): the golden block from the shop's front, then the wheels (OC)
static void store_spins_fill(const HtmlElem root) {
    char *golden = store_golden_html();
    char *wheels = store_wheels_html();
    StrBuf sb = {0};
    if (golden) sb_append(&sb, golden);
    if (wheels) sb_append(&sb, wheels);
    char *html = sb_finish(&sb);
    HtmlElem content[4];
    const int cn = html_dom_query_all(root, ".sectioned-scroll__section[data-index=\"0\"] .frvr-store-section__content", content, 4);
    for (int i = 0; i < cn; i++) html_dom_set_html(content[i], html);
    free(golden);
    free(wheels);
    free(html);
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
    store_spins_fill(root);
    if (M.shop) store_products_fill(root);
    strike_econ_get_shop(on_shop, NULL); // Qo(): the shop, again on every visit
}

// yU's exchange (Jce, the spin's shortfall): the spin tickets Mf the stepper buys with KR, one ticket
// at tickets_1's price (the shop's, else the catalogue's). The KR packs are bought in the Currency section.
#define TICKET_PRODUCT "tickets_1"

static int krunkies(void) {
    if (FIXTURE.set) return FIXTURE.kr;
    const StrikeAccount *e = econ();
    return e ? (int) e->wallet[STRIKE_CUR_KR] : 0;
}

static int ticket_price(void) {
    const cJSON *p;
    cJSON_ArrayForEach(p, cJSON_GetObjectItemCaseSensitive(M.shop, "products")) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(p, "_id"));
        if (!id || strcmp(id, TICKET_PRODUCT)) continue;
        const cJSON *price = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(p, "price"), 0);
        const int amount = (int) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(price, "amount"));
        if (amount > 0) return amount;
    }
    const StrikeProduct *sp = strike_product(TICKET_PRODUCT);
    return sp ? sp->price_amount : 80;
}

// yU / pU: the stepper's max is the tickets the KR pays for (at least 1, pU's min); the KR button shows
// price * Mf and is enabled when the account can pay Mf tickets.
static int ticket_afford(void) {
    const int price = ticket_price();
    return price > 0 ? krunkies() / price : 0;
}

static void tickets_fill(const HtmlElem root) {
    const int afford = ticket_afford();
    const int max = afford < 1 ? 1 : afford;
    if (M.ticket_n < 1) M.ticket_n = 1;
    if (M.ticket_n > max) M.ticket_n = max;
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", M.ticket_n);
    set_text(root, "#ticketAmount", buf);
    snprintf(buf, sizeof(buf), "%d", ticket_price() * M.ticket_n);
    set_text(root, "#ticketPrice", buf);
    const HtmlElem minus = html_dom_query(root, "#ticketMinus"), plus = html_dom_query(root, "#ticketPlus");
    const HtmlElem buy = html_dom_query(root, "#ticketBuy");
    if (minus) html_dom_set_class(minus, "is-disabled", M.ticket_n <= 1);
    if (plus) html_dom_set_class(plus, "is-disabled", M.ticket_n >= max);
    if (buy) html_dom_set_class(buy, "is-disabled", !(afford >= 1 && afford >= M.ticket_n));
}

// onPurchase of Jce: the popup closes whatever the answer (FRVR's yU calls it after its try / catch)
static void tickets_done(void) {
    M.ticket_busy = 0;
    if (strike_screen_top() == STRIKE_SCREEN_POPUP && M.popup == POPUP_BUY_TICKETS) strike_screen_pop(); // kt()
}

static void on_tickets_bought(void *user, const int status, const cJSON *data, const cJSON *error) {
    (void) user;
    (void) data;
    const char *msg = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(error, "message"));
    if (status == 200) printf("[strike] bought %s x%d\n", TICKET_PRODUCT, M.ticket_n);
    else printf("[strike] purchase refused (%d): %s\n", status, msg ? msg : "no answer");
    tickets_done();
}

static void buy_tickets(void) {
    const int afford = ticket_afford();
    if (M.ticket_busy || afford < 1 || afford < M.ticket_n) return; // the KR button is disabled
    M.ticket_busy = 1;
    if (!strike_econ_buy(TICKET_PRODUCT, M.ticket_n, on_tickets_bought, NULL)) {
        printf("[strike] purchase %s: no Strike account (sign in)\n", TICKET_PRODUCT);
        tickets_done();
    }
}

// ---------------------------------------------------------------- SPIN (SA / aue / oU)

static void spin_fill(const HtmlElem root) {
    const WheelView *w = current_wheel();
    if (!w) return;
    char buf[64];
    set_text(root, ".spinner-title", w->name);
    // oU: one row per reward pool, weight * 100 %
    const HtmlElem odds = html_dom_query(root, ".odds");
    if (odds) {
        StrBuf sb = {0};
        for (int i = 0; i < w->odd_count; i++) {
            sb_appendf(&sb, "<div class=\"odd\"><div class=\"odd-icon\" style=\"background: %s;\"></div><div class=\"odd-name\">", w->odds[i].color);
            sb_append_esc(&sb, w->odds[i].name);
            sb_appendf(&sb, "</div><div class=\"odd-chance\">%g%%</div></div>", w->odds[i].weight * 100.0);
        }
        char *html = sb_finish(&sb);
        html_dom_set_html(odds, html);
        free(html);
    }
    // buttonTicketsSplit: 10x the price on top (onWheelClick(e, 10)), 1x below (onWheelClick(e))
    snprintf(buf, sizeof(buf), "%d", w->amount * 10);
    set_text(root, "#spinTopSection .spin-tickets span", buf);
    snprintf(buf, sizeof(buf), "%d", w->amount);
    set_text(root, "#oneTicketSpin .spin-tickets span", buf);
    const HtmlElem top = html_dom_query(root, "#spinTopSection"), bottom = html_dom_query(root, "#spinBottomSection");
    if (top) html_dom_set_attr(top, "onclick", "strikeAction('spin-buy:10')");
    if (bottom) html_dom_set_attr(bottom, "onclick", "strikeAction('spin-buy:1')");
    // the header's currencies (A1) only for a spin-ticket wheel
    const HtmlElem header = html_dom_query(root, ".frvr-controls-right .currency-header");
    if (header) {
        if (w->ticket) fill_currency(root);
        else html_dom_remove(header);
    }
    M.wheel_f = 0.0f;
    // the golden screen (FRVR lce's meter, above the wheel); other wheels have none
    HtmlElem old = html_dom_query(root, ".golden-spin-meter");
    if (old) html_dom_remove(old);
    const HtmlElem center = html_dom_query(root, ".spin-wheel__container");
    if (center && M.spin == GOLDEN_SPIN) {
        char *meter = golden_meter_html();
        html_dom_insert_html(center, HTML_INSERT_BEFORE_BEGIN, meter);
        free(meter);
    }
}

// onSpin's answer: C = the won pool, c its weight, d[] the pointers' targets (even: 2 pi (rand + 13) +
// pi / 2, odd: pi / 2 - 2 pi (13 - rand)), A[] their speeds (.5 + random * .5, over the largest)
static void on_spun(void *user, const int status, const cJSON *data, const cJSON *error) {
    (void) user;
    if (status != 200 || !cJSON_IsObject(data)) {
        const char *msg = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(error, "message"));
        printf("[strike] spin refused (%d): %s\n", status, msg ? msg : "no answer"); // M(ee): Js(message)
        M.sp.phase = 0;
        return;
    }
    const cJSON *rewards = cJSON_GetObjectItemCaseSensitive(data, "rewards");
    const int n = cJSON_GetArraySize(rewards);
    if (n <= 0) {
        M.sp.phase = 0;
        return;
    }
    M.sp.n = n < SPIN_WHEEL_MAX_POINTERS ? n : SPIN_WHEEL_MAX_POINTERS;
    double amax = 0.0;
    for (int k = 0; k < M.sp.n; k++) {
        const cJSON *r = cJSON_GetArrayItem(rewards, k);
        const double rnd = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(r, "rand"));
        M.sp.p[k] = 0.0;
        M.sp.d[k] = k % 2 == 0 ? 2.0 * M_PI * (rnd + 13.0) + M_PI / 2.0 : M_PI / 2.0 - 2.0 * M_PI * (13.0 - rnd);
        M.sp.A[k] = 0.5 + (double) rand() / RAND_MAX * 0.5;
        if (M.sp.A[k] > amax) amax = M.sp.A[k];
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(r, "reward"), "item");
        printf("[strike] spin result %d: pool %s rand %.4f item %s\n", k, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(r, "poolId")), rnd,
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "itemId")) ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "itemId")) : "-");
    }
    for (int k = 0; k < M.sp.n; k++) M.sp.A[k] /= amax;
    // b = the results' rewards, x = heroicSpinsTriggered: the reward screen's (showRewardScreen)
    cJSON_Delete(M.sp.rewards);
    M.sp.rewards = cJSON_CreateArray();
    const cJSON *res;
    cJSON_ArrayForEach(res, rewards) {
        const cJSON *rw = cJSON_GetObjectItemCaseSensitive(res, "reward");
        if (cJSON_IsObject(rw)) cJSON_AddItemToArray(M.sp.rewards, cJSON_Duplicate(rw, 1));
    }
    M.sp.heroic = (int) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(data, "heroicSpinsTriggered"));
    const cJSON *first = cJSON_GetArrayItem(rewards, 0);
    snprintf(M.sp.pool, sizeof(M.sp.pool), "%s", cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(first, "poolId")) ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(first, "poolId")) : "");
    M.sp.c = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(first, "poolWeight"));
    M.sp.u = M.sp.f = 0.0;
    M.sp.step_ms = 0.0;
    M.sp.phase = 2;
}

static void tickets_open(void);

static void spin_buy(const int count) {
    const WheelView *w = current_wheel();
    if (!w || (M.sp.phase != 0 && M.sp.phase != 4) || strike_screen_top() != STRIKE_SCREEN_SPIN) return; // isSpinning
    if (FIXTURE.set) return; // a capture fixture's wheel is not kn_brain's
    // onSpin: a ticket wheel with fewer tickets than spins, and KR for one ticket, opens the exchange (Jce)
    // instead of spinning. FRVR tests the ticket count for every wheel; a KR wheel is paid in KR, so here
    // only the ticket wheel is gated. The KR packs are bought in the store, not here.
    if (w->ticket && count > spin_tickets() && krunkies() >= ticket_price()) {
        tickets_open();
        return;
    }
    M.sp.wheel = M.spin;
    M.sp.phase = 1;
    if (!strike_econ_spin(w->id, count, 0, on_spun, NULL)) {
        // Se.isLoggedIn || Pt(Zu): the login popup; there is no Strike login screen here
        printf("[strike] spin %s x%d: no Strike account (sign in)\n", w->id, count);
        M.sp.phase = 0;
        return;
    }
    printf("[strike] spin %s x%d\n", w->id, count);
}

// One 1/60 s step of aue.render's spin state (FRVR moves the pointers by a fixed share per frame)
static void spin_step(void) {
    if (M.sp.phase == 2) {
        int done = 1;
        for (int k = 0; k < M.sp.n; k++) {
            if (M.sp.p[k] == M.sp.d[k]) continue;
            M.sp.p[k] += (M.sp.d[k] - M.sp.p[k]) * M.sp.A[k] * 0.024;
            if (fabs(M.sp.d[k] - M.sp.p[k]) <= 0.003 / M.sp.A[k]) M.sp.p[k] = M.sp.d[k]; // onPointerSpinFinished
            else done = 0;
        }
        if (done) { // showRewardScreen: f = 100 - c * 100, then Pt(Es, {rewards: b, origin: spinWheel, heroicSpinsTriggered: x})
            M.sp.f = 100.0 - M.sp.c * 100.0;
            M.sp.u = 0.0;
            M.sp.phase = M.sp.f > 0.0 ? 3 : 4;
            printf("[strike] spin landed on %s\n", M.sp.pool);
            audio_play("reward", 0.3f, 0, 1.0f);
            strike_reward_show(M.sp.rewards, "spinWheel", M.sp.heroic, NULL, NULL);
        }
    } else if (M.sp.phase == 3) {
        // the won pool keeps growing under the reward screen (aue.render runs on); afterwards the wheel
        // shows it grown until the next spin (onSpin)
        M.sp.u += 1000.0 / 60.0 * 0.4; // u += F * .4
        if (M.sp.u >= M.sp.f) {
            M.sp.u = M.sp.f;
            M.sp.phase = 4;
        }
    }
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec * 1e-9;
}

// aue.resize + render: the canvas is F x F, F = the smaller side of its parent's content box (the
// parent is .spin-wheel__container, padded 74 * --base-scale on top and --screen-margin = 16 *
// --base-scale below, with the page's --base-scale when resize runs), the container's --base-scale
// then F / 280 px; the wheel (disc H = F / 2 * 0.95, slices at 0.95 H by weight from angle 0, the
// pointer, the centre disc 0.6 H) is spin_wheel.c's, drawn into this canvas.
static void spin_tick(void) {
    M.wheel.visible = 0;
    // the spin's frames (fixed 1/60 s steps of the wall clock, at most a quarter second at a time); they
    // run on under the reward screen
    const double t = now_s();
    if (M.sp.phase >= 2 && M.sp.phase < 4) {
        double dt = M.sp.last_s > 0.0 ? (t - M.sp.last_s) * 1000.0 : 0.0;
        if (dt > 250.0) dt = 250.0;
        M.sp.step_ms += dt;
        while (M.sp.step_ms >= 1000.0 / 60.0 && M.sp.phase >= 2 && M.sp.phase < 4) {
            M.sp.step_ms -= 1000.0 / 60.0;
            spin_step();
        }
    }
    M.sp.last_s = t;
    if (M.sp.phase >= 2 && !strike_screen_shown(STRIKE_SCREEN_SPIN)) M.sp.phase = 0; // SPIN went (home): the next one starts idle
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
    const WheelView *w = current_wheel();
    const int spinning = M.sp.phase >= 2 && M.sp.wheel == M.spin;
    const double uf = spinning && M.sp.f > 0.0 ? M.sp.u / M.sp.f : 0.0;
    // the end animation: the won pool grows by u, the others give way, the wheel turns so the won
    // pool's middle comes under the pointer (rotate(j - pi * u / f))
    double j = 0.0;
    if (spinning && M.sp.phase >= 3) {
        for (int i = 0; w && i < w->odd_count; i++) {
            if (!strcmp(w->odds[i].pool, M.sp.pool)) {
                j += w->odds[i].weight * 100.0 / 2.0;
                break;
            }
            j += w->odds[i].weight * 100.0;
        }
        j = fmod(2.0 * M_PI * (j / 100.0) * uf, 2.0 * M_PI);
        d->slice_rotation = (float) (j - M_PI * uf);
    }
    float from = 0.0f;
    for (int i = 0; w && i < w->odd_count && i < SPIN_WHEEL_MAX_SLICES; i++) {
        double x = w->odds[i].weight * 100.0;
        if (spinning && M.sp.phase >= 3) x += !strcmp(w->odds[i].pool, M.sp.pool) ? M.sp.u : -M.sp.u * (w->odds[i].weight * 100.0 / M.sp.f);
        const float span = x > 0.0 ? (float) (2.0 * M_PI * x / 100.0) : 0.0f;
        d->slice_from[i] = from;
        d->slice_to[i] = from + span;
        unsigned r = 0, g = 0, b = 0;
        sscanf(w->odds[i].color + 1, "%02x%02x%02x", &r, &g, &b);
        d->slice_rgb[i][0] = (float) r / 255.0f;
        d->slice_rgb[i][1] = (float) g / 255.0f;
        d->slice_rgb[i][2] = (float) b / 255.0f;
        from += span;
        d->slices = i + 1;
    }
    if (spinning) {
        d->pointer_count = M.sp.n;
        for (int k = 0; k < M.sp.n; k++) d->pointer_rotation[k] = (float) M.sp.p[k];
        d->pointer_scale = (float) (1.0 - uf); // V
    } else {
        d->pointer_count = 1; // p = [0]: one pointer, straight up
        d->pointer_rotation[0] = 0.0f;
        d->pointer_scale = 1.0f;
    }
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
    if (!M.board_fixture) {
        const int n = M.live_board_count < max ? M.live_board_count : max;
        memcpy(rows, M.live_board, sizeof(BoardRow) * (size_t) n);
        return n;
    }
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
    if (!M.board_fixture && M.board_cols[0][0]) { // f1e's stat titles: the board's columns
        HtmlElem titles[2];
        const int tn = html_dom_query_all(content, ".frvr-lboard-end-screen__stat-col--title", titles, 2);
        for (int i = 0; i < tn; i++) html_dom_set_text(titles[i], M.board_cols[i]);
    }
    StrBuf sb = {0};
    for (int i = 0; i < n; i++) {
        const BoardRow *r = &rows[i];
        const char *P = "frvr-lboard-end-screen__";
        sb_appendf(&sb, "<div class=\"%srow-wrapper\" style=\"--row-index: %d;\"><div class=\"%srow\">", P, i + 1, P);
        sb_appendf(&sb, "<div class=\"%sposition %sposition--%s\"><span>%d</span></div>", P, P, r->you ? "player" : "ffa",
                   M.board_fixture || r->score_rank <= 0 ? i + 1 : r->score_rank);
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
    const HtmlElem holder = html_dom_query(root, ".popup__holder");
    if (holder && M.popup == POPUP_LEADERBOARD) { // g(QM, {enemyPlayers, playerData, stats}) in the frame
        char *html = strike_template("LEADERBOARD_PLAYERS", NULL, 0);
        if (html) {
            html_dom_set_html(holder, html);
            free(html);
        }
        leaderboard_fill(holder);
    } else if (holder && M.popup == POPUP_PRODUCT) { // g(U2, {product, ...}): YA's click is onPurchase (kt) then $r
        char action[40];
        snprintf(action, sizeof(action), "buy-confirm:%d", M.product);
        char *html = strike_product_popup_html(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(M.shop, "products"), M.product), action);
        html_dom_set_html(holder, html ? html : "");
        free(html);
    } else if (holder && M.popup == POPUP_BUY_TICKETS) { // Jce: yU, the ticket exchange in Sr
        char *html = strike_template("BUY_TICKETS", NULL, 0);
        if (html) {
            html_dom_set_html(holder, html);
            free(html);
        }
        tickets_fill(root);
    }
}

static void popup_open(const PopupKind kind) {
    M.popup = kind;
    if (strike_screen_shown(STRIKE_SCREEN_POPUP)) strike_screen_rerender(STRIKE_SCREEN_POPUP);
    else strike_screen_push(STRIKE_SCREEN_POPUP); // Pt(In, {builder}, {preventReplace: true})
}

// Jce(): Pt(In, {builder: yU}, {preventReplace, trackingId: "popup_spin_purchase"})
static void tickets_open(void) {
    M.ticket_busy = 0;
    popup_open(POPUP_BUY_TICKETS);
}

// ---------------------------------------------------------------- CUSTOMIZATION (Ly / hde) and T5

// Ly.categories: the boxes by title (windows.loadout.*), their item type ($i) and FRVR's loadout key.
// Skin Color (index skinColIndex, itemType undefined: CAT_SKIN_COLOR) opens T5's colour list.
#define CAT_SKIN_COLOR (-1)
static const struct { const char *title; int type; } CATS[] = {
    {"Weapon", STRIKE_ITEM_WEAPON}, {"Head", STRIKE_ITEM_HAT}, {"Back", STRIKE_ITEM_BACK}, {"Melee", STRIKE_ITEM_MELEE},
    {"Clothing", STRIKE_ITEM_DYE}, {"Waist", STRIKE_ITEM_WAIST}, {"Face", STRIKE_ITEM_FACE}, {"Body", STRIKE_ITEM_CHEST},
    {"Skin Color", CAT_SKIN_COLOR},
};
#define CAT_COUNT ((int) (sizeof(CATS) / sizeof(CATS[0])))

// xe.skinColors (Q8): FRVR's six, the first six of Krunker's (ui_loadout.c SKIN_COLORS, the same values)
static const char *const SKIN_COLOR_NAMES[] = {"Bronze", "Sienna", "Rose", "Tan", "Pale", "Infected"};
#define SKIN_COLOR_COUNT ((int) (sizeof(SKIN_COLOR_NAMES) / sizeof(SKIN_COLOR_NAMES[0])))

// ade(): the selected skin colour, 1 (Sienna) while none is saved (loadout.skinColIndex -1)
static int skin_color_index(void) {
    const int i = ui_loadout_saved("skinColIndex");
    return i >= 0 && i < SKIN_COLOR_COUNT ? i : 1;
}

// I5(): the selected class's primary weapon (its skin slot w<weapon>, strike_econ_apply_look)
static int class_weapon(void) {
    const int id = CLASSES[current_class()].id;
    const ClassConfig *c = rules_classes();
    return c && id >= 0 && id < (int) rules_class_count() && c[id].loadout ? c[id].loadout[0] : 0;
}

// The loadout slot of a category ("w<weapon>", "t<type>"; "" when the type does not equip)
static void cat_slot(const int cat, char *out, const size_t n) {
    out[0] = 0;
    if (CATS[cat].type == STRIKE_ITEM_WEAPON) snprintf(out, n, "w%d", class_weapon());
    else if (CATS[cat].type != STRIKE_ITEM_CHEST && CATS[cat].type != CAT_SKIN_COLOR) snprintf(out, n, "t%d", CATS[cat].type);
}

// hde: the box's label / image follow the equipped item (Default without one)
static void customization_fill(const HtmlElem root) {
    HtmlElem boxes[24];
    const int n = html_dom_query_all(root, ".customization-screen__cat-box", boxes, 24);
    for (int i = 0; i < n; i++) {
        const HtmlElem t = html_dom_query(boxes[i], ".customization-screen__cat-box-title span");
        char title[48] = "";
        if (t) html_dom_get_text(t, title, sizeof(title));
        int cat = -1;
        for (int c = 0; c < CAT_COUNT; c++) {
            if (!strcmp(CATS[c].title, title)) cat = c;
        }
        if (cat < 0) continue;
        char hook[40], slot[16];
        snprintf(hook, sizeof(hook), "strikeAction('item-slot:%d')", cat); // Pt(T5, {itemDef, index, title, itemType})
        html_dom_set_attr(boxes[i], "onclick", hook);
        if (CATS[cat].type == CAT_SKIN_COLOR) { // the colour square and its name (skinColors[skinColIndex])
            const int sc = skin_color_index();
            char bg[32];
            snprintf(bg, sizeof(bg), "#%s", ui_loadout_skin_color_hex(sc));
            const HtmlElem sq = html_dom_query(boxes[i], ".customization-screen__skin-color");
            if (sq) html_dom_set_style(sq, "background", bg);
            const HtmlElem label = html_dom_query(boxes[i], ".customization-screen__cat-box-label span");
            if (label) {
                html_dom_set_text(label, SKIN_COLOR_NAMES[sc]);
                char id[48];
                snprintf(id, sizeof(id), "%s-span", SKIN_COLOR_NAMES[sc]);
                html_dom_set_attr(label, "id", id);
            }
            continue;
        }
        cat_slot(cat, slot, sizeof(slot));
        const char *equipped = slot[0] ? strike_econ_equipped(slot) : NULL;
        const StrikeItem *it = equipped ? strike_item(equipped) : NULL;
        const HtmlElem label = html_dom_query(boxes[i], ".customization-screen__cat-box-label span");
        if (label) html_dom_set_text(label, it ? it->name : "Default");
        const HtmlElem img = html_dom_query(boxes[i], ".customization-screen__cat-box-middle img");
        if (img) {
            char cur[256] = "", orig[256] = "";
            html_dom_get_attr(img, "src", cur, sizeof(cur));
            if (html_dom_get_attr(img, "data-kn-default", orig, sizeof(orig)) < 0) {
                html_dom_set_attr(img, "data-kn-default", cur);
                snprintf(orig, sizeof(orig), "%s", cur);
            }
            char src[512];
            if (it && it->skin >= 0) store_skin_preview(it->skin, src, sizeof(src));
            else snprintf(src, sizeof(src), "%s", orig);
            html_dom_set_attr(img, "src", src);
        }
    }
}

// state.strike.itemSelection of a --render-scenario (strike_menu_scenario_state): the owned items and the
// equipped ones T5 shows instead of the account's (sample data)
static struct {
    int set;
    cJSON *owned;    // [{itemId}] as /players/me/items rows
    cJSON *equipped; // {slot: itemId}
} ITEMS;

static const cJSON *item_selection_items(void) { return ITEMS.set ? ITEMS.owned : strike_econ_items(); }

static const char *item_selection_equipped(const char *slot) {
    if (!ITEMS.set) return strike_econ_equipped(slot);
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(ITEMS.equipped, slot));
}

static int item_rarity_cmp(const void *pa, const void *pb) {
    const StrikeItem *a = strike_item((const char *) pa), *b = strike_item((const char *) pb);
    return (b ? b->rarity : 0) - (a ? a->rarity : 0); // rarity, highest first
}

// T5: the account's items of the category (skin.type, the class weapon's for Weapon) by rarity, then
// "Default"; rde: the square of the equipped one (none: Default) is --selected, a click equips
static void item_selection_fill(const HtmlElem root) {
    const int cat = M.item_cat;
    const HtmlElem title = html_dom_query(root, ".frvr-window-title");
    if (title) html_dom_set_text(title, CATS[cat].title);
    const HtmlElem grid = html_dom_query(root, ".item-selection__items-grid");
    if (CATS[cat].type == CAT_SKIN_COLOR) { // T5 without an itemType: xe.skinColors, the selected one ade()
        StrBuf sb = {0};
        const int sc = skin_color_index();
        M.item_count = 0;
        for (int i = 0; i < SKIN_COLOR_COUNT; i++) {
            char index[16];
            snprintf(index, sizeof(index), "%d", i);
            snprintf(M.item_ids[M.item_count++], sizeof(M.item_ids[0]), "%d", i);
            const StrikeVar v[] = {{"selected", i == sc ? " item-selection__item-square--selected" : ""}, {"index", index},
                                   {"color", ui_loadout_skin_color_hex(i)}, {"name", SKIN_COLOR_NAMES[i]}};
            char *html = strike_template_part("ITEM_SELECTION_SCREEN", "skin", v, 4);
            if (html) sb_append(&sb, html);
            free(html);
        }
        char *items = sb_finish(&sb);
        if (grid) html_dom_set_html(grid, items ? items : "");
        free(items);
        return;
    }
    char slot[16];
    cat_slot(cat, slot, sizeof(slot));
    const int weapon = class_weapon();
    M.item_count = 0;
    const cJSON *row;
    cJSON_ArrayForEach(row, item_selection_items()) {
        if (M.item_count >= 63) break;
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, "itemId"));
        const StrikeItem *it = id ? strike_item(id) : NULL;
        if (!it || it->is_default || it->type != CATS[cat].type) continue;
        if (it->type == STRIKE_ITEM_WEAPON && it->weapon != weapon) continue;
        snprintf(M.item_ids[M.item_count++], sizeof(M.item_ids[0]), "%s", id);
    }
    qsort(M.item_ids, (size_t) M.item_count, sizeof(M.item_ids[0]), item_rarity_cmp);
    M.item_ids[M.item_count++][0] = 0; // the Default square
    const char *equipped = slot[0] ? item_selection_equipped(slot) : NULL;
    StrBuf sb = {0};
    for (int i = 0; i < M.item_count; i++) {
        const StrikeItem *it = M.item_ids[i][0] ? strike_item(M.item_ids[i]) : NULL;
        const int selected = it ? equipped && !strcmp(equipped, M.item_ids[i]) : !equipped;
        char image[512], index[16], rarity[48], bonus[16];
        if (it && it->skin >= 0) store_skin_preview(it->skin, image, sizeof(image));
        else snprintf(image, sizeof(image), "%s", M.item_default_img);
        snprintf(index, sizeof(index), "%d", i);
        // the label's class: item-rarity-banner--<rarity>, "" for Default (Mithril keeps the joining space)
        if (it && it->rarity >= 0) snprintf(rarity, sizeof(rarity), " item-rarity-banner--%d", it->rarity);
        else snprintf(rarity, sizeof(rarity), " ");
        const char *img = NULL;
        char *stat = NULL;
        if (it && strike_item_power(it->id, bonus, sizeof(bonus), &img)) { // wc {labelItems, class: item-selection__item-stat}
            const StrikeVar sv[] = {{"bonus", bonus}, {"img", img}};
            stat = strike_template_part("ITEM_SELECTION_SCREEN", "stat", sv, 2);
        }
        const StrikeVar v[] = {{"selected", selected ? " item-selection__item-square--selected" : ""}, {"index", index},
                               {"image", image}, {"stat", stat ? stat : ""}, {"rarity", rarity}, {"name", it ? it->name : "Default"}};
        char *html = strike_template_part("ITEM_SELECTION_SCREEN", "item", v, 6);
        if (html) sb_append(&sb, html);
        free(html);
        free(stat);
    }
    char *items = sb_finish(&sb);
    if (grid) html_dom_set_html(grid, items ? items : "");
    free(items);
}

static void on_equipped(void *user, const int status, const cJSON *data, const cJSON *error) {
    (void) user;
    (void) data;
    const char *msg = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(error, "message"));
    if (status == 200) printf("[strike] equipped (%s)\n", M.item_cat >= 0 && M.item_cat < CAT_COUNT ? CATS[M.item_cat].title : "?");
    else printf("[strike] equip refused (%d): %s\n", status, msg ? msg : "no answer");
}

static void item_pick(const int index) {
    if (index < 0 || index >= M.item_count) return;
    if (CATS[M.item_cat].type == CAT_SKIN_COLOR) { // rde: loadout.skinColIndex = r, saved (ac("loadout.skinColIndex"))
        if (index == skin_color_index()) return;
        ui_loadout_select_skin_color(index);
        printf("[strike] skin color %d (%s)\n", index, SKIN_COLOR_NAMES[index]);
        HtmlElem root;
        if ((root = strike_screen_root(STRIKE_SCREEN_ITEM_SELECTION))) item_selection_fill(root);
        if ((root = strike_screen_root(STRIKE_SCREEN_CUSTOMIZATION))) customization_fill(root);
        return;
    }
    char slot[16];
    cat_slot(M.item_cat, slot, sizeof(slot));
    if (!slot[0]) return;
    const char *equipped = strike_econ_equipped(slot);
    const char *id = M.item_ids[index];
    if (id[0] ? equipped && !strcmp(equipped, id) : !equipped) return; // o: already the selected one
    const int sent = id[0] ? strike_econ_equip(id, on_equipped, NULL) : strike_econ_unequip(slot, on_equipped, NULL);
    if (!sent) printf("[strike] equip %s: no Strike account (sign in)\n", id[0] ? id : slot);
}

// ---------------------------------------------------------------- leaderboards (strike_econ)

// GET /strike/v1/leaderboards/:board: {board, players: [{rank, playerId, name, value, level}], me}. FRVR
// shows player boards only in its tournament rewards popup (YB in In); KrunkNative has no hosted
// tournaments, so a board opens in the same frame from strikeAction('leaderboard[:xp|kills|wins|
// score|games]') (no FRVR button reaches it). Columns: Level and the board's value.
static void on_board(void *user, const int status, const cJSON *data, const cJSON *error) {
    (void) user;
    (void) error;
    if (status != 200 || !cJSON_IsObject(data)) {
        printf("[strike] leaderboard refused (%d)\n", status);
        return;
    }
    const AccountInfo *acc = active_account();
    const char *board = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(data, "board"));
    M.live_board_count = 0;
    const cJSON *row;
    cJSON_ArrayForEach(row, cJSON_GetObjectItemCaseSensitive(data, "players")) {
        if (M.live_board_count >= BOARD_MAX) break;
        BoardRow *r = &M.live_board[M.live_board_count++];
        memset(r, 0, sizeof(*r));
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, "name"));
        snprintf(r->name, sizeof(r->name), "%s", name ? name : "");
        r->has_stats = 1;
        r->score = (int) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(row, "level"));
        r->kills = (int) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(row, "value"));
        r->score_rank = (int) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(row, "rank"));
        const char *pid = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, "playerId"));
        r->you = acc && pid && atoi(pid) == acc->id;
    }
    snprintf(M.board_cols[0], sizeof(M.board_cols[0]), "Level");
    static const char *const NAMES[][2] = {{"xp", "XP"}, {"kills", "Kills"}, {"wins", "Wins"}, {"score", "Score"}, {"games", "Games"}};
    snprintf(M.board_cols[1], sizeof(M.board_cols[1]), "%s", board ? board : "");
    for (size_t i = 0; board && i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        if (!strcmp(board, NAMES[i][0])) snprintf(M.board_cols[1], sizeof(M.board_cols[1]), "%s", NAMES[i][1]);
    }
    printf("[strike] leaderboard %s: %d players\n", board ? board : "?", M.live_board_count);
    popup_open(POPUP_LEADERBOARD);
}

// ---------------------------------------------------------------- CLASS_SELECTION (B5 / ode / lde)

// A class's `unlockable` (To, the bundle's bs list): Hunter from a Bundle, Run N Gun and Rocketeer from the
// Rewards Track, Shotgunner and Detective from Spin tickets; Triggerman has none (a starter class).
typedef enum { UNLOCK_NONE, UNLOCK_SPIN_TICKETS, UNLOCK_REWARDS_TRACK, UNLOCK_BUNDLE } ClassUnlock;

static ClassUnlock class_unlockable(const int class_id) {
    switch (class_id) {
    case 1: return UNLOCK_BUNDLE;
    case 2:
    case 7: return UNLOCK_REWARDS_TRACK;
    case 4:
    case 5: return UNLOCK_SPIN_TICKETS;
    default: return UNLOCK_NONE;
    }
}

// ude(unlockable): the line under a locked class
static const char *class_unlock_text(const int class_id) {
    switch (class_unlockable(class_id)) {
    case UNLOCK_SPIN_TICKETS: return "Unlockable from Spin tickets";
    case UNLOCK_REWARDS_TRACK: return "Unlockable from the Rewards Track";
    case UNLOCK_BUNDLE: return "Unlockable from a Bundle purchase";
    default: return "";
    }
}

// the account owns the class (FRVR's classAvailable over Se.account.classes; a guest owns Triggerman)
static int class_owned(const int slot) { return strike_econ_class_owned(CLASSES[slot].id); }

// cde(unlockable): the spin tickets and bundles open STORE, the Rewards Track opens PROFILE
static void class_unlock_open(void) {
    switch (class_unlockable(CLASSES[M.previewed].id)) {
    case UNLOCK_REWARDS_TRACK:
        strike_screen_push(STRIKE_SCREEN_PROFILE); // Pt(pg): pg is the profile (YC)
        break;
    case UNLOCK_SPIN_TICKETS:
    case UNLOCK_BUNDLE:
        M.store_section = 0;                       // Pt(cc())
        strike_screen_push(STRIKE_SCREEN_STORE);
        break;
    default:
        break;
    }
}

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
    // the class's box (FRVR's .class-selection__select-class): a class the account owns gets _t,
    // "Selected" (disabled) for the class in use and "Select" otherwise; a locked class gets the
    // unlock line instead (ude), which opens the store or the profile when tapped (cde)
    const HtmlElem box = html_dom_query(root, ".class-selection__select-class");
    if (!box) return;
    char html[512];
    if (class_owned(M.previewed)) {
        const int in_use = M.previewed == current_class();
        snprintf(html, sizeof(html),
                 "<div class=\"frvr-button frvr-button--size-sm frvr-button--width-170%s\"%s onclick=\"strikeAction('select-class')\">"
                 "<span class=\"frvr-button-text\">%s</span></div>",
                 in_use ? " is-disabled" : "", in_use ? " disabled=\"\"" : "", in_use ? "Selected" : "Select");
    } else {
        snprintf(html, sizeof(html),
                 "<div class=\"class-selection__unlock-info\" onclick=\"strikeAction('unlock-info')\"><span>%s</span></div>",
                 class_unlock_text(CLASSES[M.previewed].id));
    }
    html_dom_set_html(box, html);
}

// ---------------------------------------------------------------- SETTINGS (Dy): values

// The rows of the Display / Game / Sound tabs by label (FRVR's settings list: settings.quality.*,
// settings.mobile.*, settings.audio.*; the key is its setting key, the default the capture's value
// on a fresh guest, tools/uidiff/scenarios/strike/settings-*.json). Values live in the shared
// settings store (ui_settings.c) so the audio sliders drive client_apply_setting like v10's.
typedef struct { const char *label, *key; double def; } StrikeSetting;
static const StrikeSetting SETTING_ROWS[] = {
    {"Resolution", "resolution", 0.4}, {"Frame Cap", "updateRate", 0}, {"Low Spec", "lowSpec", 0}, {"Particles", "particles", 1},
    {"Particle Distance", "particlesDist", 600}, {"Texture Animations", "textureAnim", 1}, {"Object Animations", "objectAnim", 1},
    {"No Textures", "noTex", 0}, {"Map Details", "mapDet", 1}, // Box Office's keys (ui_render_settings.c RS_TABLE)
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

// The Display tab's V-Sync row (Box Office's "vsync", ui_client_settings.c): not one of FRVR's rows, so
// settings_insert_vsync adds it after Frame Cap with FRVR's row markup. It is a store row like the others
// (client_apply_setting -> the swap interval re-syncs in client_render_frame_end), but no preset sets it and
// a change leaves the preset as it is (FRVR's presets set FRVR's rows only).
static const StrikeSetting VSYNC_ROW = {"V-Sync", "vsync", 1};
static int is_native_row(const char *key) {
    return !strcmp(key, VSYNC_ROW.key);
}

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

// ---------------------------------------------------------------- SETTINGS presets (jue / selectSettingPre)

// settings.list[k].pro / .perf in the 1.37.2 bundle (index-Cz2uYXJQ.deob.js 146940-150560, read as text):
// the value a preset sets. A setting without one takes its default (zt.list[a][pro|perf] ?? def), so
// the Pro and Performance presets set every row of SETTING_ROWS; NAN = none.
typedef struct { const char *key; double pro, perf; } StrikePreset;
static const StrikePreset PRESET_VALUES[] = {
    {"resolution", 1, 0.4}, {"lowSpec", NAN, 1}, {"particles", 0, 0}, {"textureAnim", NAN, 0},
    {"objectAnim", NAN, 0}, {"hitIndicators", 1, 0}, {"showFPS", 1, 0}, {"sound", 0.1, 0.1},
    {"fov", 110, NAN}, {"fpsFOV", 105, NAN},
    {"updateRate", NAN, 60}, // Frame Cap: the Default and Pro presets take the row's default (0, uncapped); Performance caps at 60
};

static double preset_value(const StrikeSetting *r, const int id) {
    for (size_t i = 0; i < sizeof(PRESET_VALUES) / sizeof(PRESET_VALUES[0]); i++) {
        const StrikePreset *p = &PRESET_VALUES[i];
        if (strcmp(p->key, r->key)) continue;
        const double v = id == PRESET_PRO ? p->pro : id == PRESET_PERFORMANCE ? p->perf : NAN;
        if (!isnan(v)) return v;
    }
    return r->def;
}

// The rows that are switches (checkboxes on the Display / Game / Sound tabs): stored as bools.
static int is_switch(const char *key) {
    static const char *const SWITCHES[] = {"lowSpec", "particles", "textureAnim", "objectAnim", "hitIndicators", "showPing",
                                           "showFPS", "autoFireMbl", "aimAssist", "invertYMbl", "vibrationMbl", "gradSpeedMbl",
                                           "mob_ctrl_disable_tap_jump", "leftHanded", "noTex", "mapDet", "vsync"};
    for (size_t i = 0; i < sizeof(SWITCHES) / sizeof(SWITCHES[0]); i++) {
        if (!strcmp(SWITCHES[i], key)) return 1;
    }
    return 0;
}

// the store's value of a row (its default when the page has none, as the row shows it)
static double key_number(const char *key) {
    for (size_t i = 0; i < sizeof(SETTING_ROWS) / sizeof(SETTING_ROWS[0]); i++) {
        if (!strcmp(SETTING_ROWS[i].key, key)) return setting_number(&SETTING_ROWS[i]);
    }
    return 0.0;
}

static void preset_store(const StrikeSetting *r, const double v) {
    HtmlArg a = {0};
    a.kind = is_switch(r->key) ? HTML_ARG_BOOL : HTML_ARG_NUM;
    a.num = v;
    ui_settings_set(r->key, &a);
}

// krk_customPre: every row's value, kept when Custom is left
static void preset_save_custom(void) {
    cJSON *o = cJSON_CreateObject();
    for (size_t i = 0; o && i < sizeof(SETTING_ROWS) / sizeof(SETTING_ROWS[0]); i++) {
        const double v = setting_number(&SETTING_ROWS[i]);
        if (is_switch(SETTING_ROWS[i].key)) cJSON_AddBoolToObject(o, SETTING_ROWS[i].key, v != 0.0);
        else cJSON_AddNumberToObject(o, SETTING_ROWS[i].key, v);
    }
    char *json = o ? cJSON_PrintUnformatted(o) : NULL;
    if (json) html_storage_set("strikeCustomPre", json);
    printf("[strike] custom saved: resolution %g, fov %g\n", key_number("resolution"), key_number("fov"));
    free(json);
    cJSON_Delete(o);
}

// Custom: the values saved when it was last left (selectSettingPre(2) with !e)
static void preset_restore_custom(void) {
    char buf[4096];
    if (html_storage_get("strikeCustomPre", buf, sizeof(buf)) < 0) return;
    cJSON *o = cJSON_Parse(buf);
    for (size_t i = 0; o && i < sizeof(SETTING_ROWS) / sizeof(SETTING_ROWS[0]); i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, SETTING_ROWS[i].key);
        if (cJSON_IsBool(v)) preset_store(&SETTING_ROWS[i], cJSON_IsTrue(v) ? 1.0 : 0.0);
        else if (cJSON_IsNumber(v)) preset_store(&SETTING_ROWS[i], v->valuedouble);
    }
    cJSON_Delete(o);
    printf("[strike] custom restored: resolution %g, fov %g\n", key_number("resolution"), key_number("fov"));
}

static void preset_save_index(void) {
    char idx[16];
    snprintf(idx, sizeof(idx), "%d", M.preset);
    html_storage_set("strikePreIndex", idx);
}

// selectSettingPre(i) from a preset button (no confirm: the page passes t = true). Default / Pro /
// Performance set their values through the settings store (so each one reaches the client as a
// manual change would); leaving Custom saves the values first, entering Custom restores them.
static void preset_select(const int id) {
    if (id == M.preset) return;
    if (id == PRESET_CUSTOM) {
        preset_restore_custom();
    } else {
        if (M.preset == PRESET_CUSTOM) preset_save_custom();
        for (size_t i = 0; i < sizeof(SETTING_ROWS) / sizeof(SETTING_ROWS[0]); i++)
            preset_store(&SETTING_ROWS[i], preset_value(&SETTING_ROWS[i], id));
    }
    M.preset = id;
    preset_save_index();
    printf("[strike] preset %d: resolution %g, fov %g\n", id, key_number("resolution"), key_number("fov"));
}

// a setting changed by hand: the preset becomes Custom (selectSettingPre(2, true): nothing is restored)
static void preset_mark_custom(void) {
    if (M.preset == PRESET_CUSTOM) return;
    M.preset = PRESET_CUSTOM;
    preset_save_index();
}

static void preset_load(void) {
    char buf[16];
    const int saved = html_storage_get("strikePreIndex", buf, sizeof(buf)) >= 0 && buf[0] ? atoi(buf) : PRESET_CUSTOM;
    M.preset = saved >= PRESET_DEFAULT && saved <= PRESET_PERFORMANCE ? saved : PRESET_CUSTOM;
}

// the active preset button (frvr-preset__button--active)
static void presets_mark(const HtmlElem root) {
    HtmlElem presets[4];
    const int pn = html_dom_query_all(root, ".frvr-preset__button", presets, 4);
    for (int i = 0; i < pn && i < 4; i++) html_dom_set_class(presets[i], "frvr-preset__button--active", BUTTON_PRESET[i] == M.preset);
}

// window.setSetting(key, value) from a slider / switch; the number box and the range follow
static int h_strike_setting(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user;
    if (argc < 1 || args[0].kind != HTML_ARG_STR || !args[0].str) return 1;
    const char *key = args[0].str;
    // a hand change: the preset reads Custom (sets with the setSetting's b flag off); V-Sync is no preset row
    if (!is_native_row(key)) preset_mark_custom();
    char buf[32];
    if (ev->target && !strcmp(ev->type, "click")) {
        if (is_native_row(key)) { // V-Sync is stored as a bool, as Box Office's switch stores it
            HtmlArg on = {0};
            on.kind = HTML_ARG_BOOL;
            on.num = html_dom_get_checked(ev->target) ? 1.0 : 0.0;
            ui_settings_set(key, &on);
            return 0;
        }
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

// ---------------------------------------------------------------- SETTINGS: Damage Number Display (a dropdown)

// The Display tab's dropdown (FRVR: settings.interface.damageNumberDisplayMode, options Stacking /
// Individual / Legacy). Its value is the store's damageNumberDisplayMode, which strike_hud.c reads when a hit
// makes a number (strike_hud_fx.h). Default Legacy: FRVR's damage_numbers_display_mode feature gives
// default_damage_numbers_mode 2 to a player who has not chosen one (the 1.37.2 config in doc/ref); the
// bundle's own val 0 (Stacking) only holds where that feature is absent.
#define DMG_ROW_LABEL "Damage Number Display"
#define DMG_MODE_COUNT 3
static int g_dmg_open; // the options list is open

static int damage_mode_value(void) {
    double v = 0.0;
    if (!ui_setting_number("damageNumberDisplayMode", &v)) return STRIKE_DMG_LEGACY;
    return v >= 0.0 && v < DMG_MODE_COUNT ? (int) v : STRIKE_DMG_LEGACY;
}

// the row's dropdown: the header with the mode's label, and the options while the list is open
static void damage_dropdown(const HtmlElem row) {
    const HtmlElem box = html_dom_query(row, ".settVal");
    if (!box) return;
    const HtmlElem old = html_dom_query(box, ".dropdown-container");
    if (old) html_dom_remove(old);
    const int mode = damage_mode_value();
    StrBuf sb = {0};
    sb_append(&sb, "<div tabindex=\"0\" class=\"dropdown-container\">");
    sb_appendf(&sb, "<div class=\"dropdown-header\" onclick=\"strikeAction('dmgnum-toggle')\"><span>%s</span>"
                    "<div class=\"material-icons\">%s</div></div>",
               strike_damage_mode_label(mode), g_dmg_open ? "expand_less" : "expand_more");
    if (g_dmg_open) {
        sb_append(&sb, "<div class=\"dropdown-options-wrapper\"><div class=\"dropdown-options\">");
        for (int i = 0; i < DMG_MODE_COUNT; i++) {
            sb_appendf(&sb, "<div class=\"dropdown-option\" onclick=\"strikeAction('dmgnum:%d')\">%s</div>", i,
                       strike_damage_mode_label(i));
        }
        sb_append(&sb, "</div></div>");
    }
    sb_append(&sb, "</div>");
    char *html = sb_finish(&sb);
    if (html) html_dom_insert_html(box, HTML_INSERT_BEFORE_END, html);
    free(html);
}

// the dropdown on the open Settings page (after a click on its header or an option)
static void damage_row_refresh(void) {
    const HtmlElem root = strike_screen_root(STRIKE_SCREEN_SETTINGS);
    if (!root) return;
    HtmlElem rows[24];
    const int n = html_dom_query_all(root, ".setting", rows, 24);
    for (int i = 0; i < n; i++) {
        const HtmlElem name = html_dom_query(rows[i], ".settName");
        char label[96] = "";
        if (name) html_dom_get_text(name, label, sizeof(label));
        if (!strcmp(label, DMG_ROW_LABEL)) damage_dropdown(rows[i]);
    }
}

// ---------------------------------------------------------------- SETTINGS: Display tab's window rows

// The Fullscreen Mode button (FRVR's row, no handler in its markup): strikeAction('fullscreen') toggles the
// window (F11's toggle: ui_render_settings.c client_render_toggle_fullscreen); its text is the mode it leads to.
#define FULLSCREEN_ROW_LABEL "Fullscreen Mode"
static const char *fullscreen_label(void) {
    return client_render_window_mode() != RS_WINDOWED ? I18N("kn.strike.settings.fullscreen.exit", "Exit Fullscreen")
                                                      : I18N("kn.strike.settings.fullscreen.enter", "Enter Fullscreen");
}

static void fullscreen_row(const HtmlElem row) {
    const HtmlElem button = html_dom_query(row, ".frvr-button");
    const HtmlElem text = html_dom_query(row, ".frvr-button-text");
    if (button) html_dom_set_attr(button, "onclick", "strikeAction('fullscreen')");
    if (text) html_dom_set_text(text, fullscreen_label());
}

// The V-Sync row goes after Frame Cap, in FRVR's row markup (<div class="setting"> with a .switch); the
// checkbox's hook is the setSetting handler of the other switches (h_strike_setting, is_native_row).
static void settings_insert_vsync(const HtmlElem content) {
    HtmlElem rows[24];
    const int n = html_dom_query_all(content, ".setting", rows, 24);
    for (int i = 0; i < n; i++) {
        const HtmlElem name = html_dom_query(rows[i], ".settName");
        char label[96] = "";
        if (name) html_dom_get_text(name, label, sizeof(label));
        if (strcmp(label, "Frame Cap")) continue;
        StrBuf sb = {0};
        const int on = setting_number(&VSYNC_ROW) != 0.0;
        sb_append(&sb, "<div id=\"\" class=\"setting\" style=\"display: flex;\"><div title=\"");
        sb_append_esc(&sb, I18N("kn.client.vsync.info", "Wait for the display refresh (swap interval 1)"));
        sb_append(&sb, "\" class=\"settName\">");
        sb_append_esc(&sb, I18N("kn.client.vsync.name", "V-Sync"));
        sb_appendf(&sb, "</div><div class=\"settVal\"><label class=\"switch\"><input type=\"checkbox\" id=\"knVsync\"%s "
                        "onclick=\"strikeSetting('%s')\"><span class=\"switch__slider\"><span class=\"switch__grooves\"></span>"
                        "</span></label></div></div>",
                   on ? " checked=\"\"" : "", VSYNC_ROW.key);
        char *html = sb_finish(&sb);
        if (html) html_dom_insert_html(rows[i], HTML_INSERT_AFTER_END, html);
        free(html);
        return;
    }
}

// the rows of a tab's markup take their values from the store (FRVR: SETTINGS.list[i].val)
static void settings_values(const HtmlElem content) {
    HtmlElem rows[24];
    const int n = html_dom_query_all(content, ".setting", rows, 24);
    for (int i = 0; i < n; i++) {
        const HtmlElem name = html_dom_query(rows[i], ".settName");
        char label[96] = "";
        if (name) html_dom_get_text(name, label, sizeof(label));
        if (!strcmp(label, DMG_ROW_LABEL)) {
            damage_dropdown(rows[i]);
            continue;
        }
        if (!strcmp(label, FULLSCREEN_ROW_LABEL)) {
            fullscreen_row(rows[i]);
            continue;
        }
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
    g_dmg_open = 0; // a tab change closes the dropdown
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
    if (!strcmp(SETTINGS_TABS[tab], "display")) settings_insert_vsync(content);
    if (tab == 0) presets_mark(content);
}

// ---------------------------------------------------------------- KR limit info bubble (M5 / eM)

// The info icon beside the KR counter (M5's .frvr-kr-limit__info) toggles a speech bubble inside the
// counter's holder: "There's a limit to the KRs you can get each day from playing matches." It hides
// after 5 s (bubbleDuration) or when tapped (mm(type, false)). Its border image is not in FRVR's tree.
static void kr_bubble_hide(void) {
    if (M.kr_bubble.timer) html_timer_cancel(M.kr_bubble.timer);
    M.kr_bubble.timer = 0;
    M.kr_bubble.open = 0;
    // the bubble sits in the counter of the menu it was opened on (the screen on top may be another one by now)
    const StrikeScreen menus[] = {STRIKE_SCREEN_MAIN_MENU, STRIKE_SCREEN_TAP_TO_PLAY};
    for (size_t i = 0; i < sizeof(menus) / sizeof(menus[0]); i++) {
        const HtmlElem root = strike_screen_root(menus[i]);
        const HtmlElem bubble = root ? html_dom_query(root, ".speech-bubble-container") : 0;
        if (bubble) html_dom_remove(bubble);
    }
}

static void kr_bubble_timeout(void *user) {
    (void) user;
    M.kr_bubble.timer = 0; // the timer fired: nothing left to cancel
    kr_bubble_hide();
}

static void kr_bubble_place(const HtmlElem root) {
    const HtmlElem holder = root ? html_dom_query(root, ".frvr-kr-limit__holder") : 0;
    if (!holder || html_dom_query(holder, ".speech-bubble-container")) return;
    html_dom_insert_html(holder, HTML_INSERT_BEFORE_END,
                         "<div id=\"mobSpeechBubble\" class=\"speech-bubble-container\" onclick=\"strikeAction('kr-limit-bubble-close')\">"
                         "<div class=\"speech-bubble speech-bubble-kr-limit speech-bubble-flipped\">"
                         "There's a limit to the KRs you can get each day from playing matches.</div></div>");
}

// a menu screen with the KR counter was rendered: its info icon opens the bubble, and a bubble that
// is still up (its timer runs) comes back on the new render
static void kr_limit_hook(const HtmlElem root) {
    const HtmlElem info = root ? html_dom_query(root, ".frvr-kr-limit__info") : 0;
    if (info) html_dom_set_attr(info, "onclick", "strikeAction('kr-limit-info')");
    if (M.kr_bubble.open) kr_bubble_place(root);
}

static void kr_bubble_toggle(void) {
    if (M.kr_bubble.open) {
        kr_bubble_hide();
        return;
    }
    M.kr_bubble.open = 1;
    kr_bubble_place(strike_screen_root(strike_screen_top()));
    M.kr_bubble.timer = html_timer_add(5000.0, kr_bubble_timeout, NULL);
}

// ---------------------------------------------------------------- SERVER_PASSWORD (KrunkNative)

// A server refused the join for its password (ERROR 004005): v10's gameCode popup in Strike's prompt frame
// (SERVER_PASSWORD.html, the ids v10 uses). Submit joins the same game again with the typed password
// (servers_ui_join_password: a fresh ticket from the director, else the same address); Leave goes back
// (servers_ui_retry: a seek, or nothing for a direct server).
void strike_menu_server_password(void) {
    printf("[strike] the game needs a password\n");
    strike_screen_push(STRIKE_SCREEN_SERVER_PASSWORD);
}

static void server_password_error(const char *message) {
    const HtmlElem root = strike_screen_root(STRIKE_SCREEN_SERVER_PASSWORD);
    const HtmlElem group = root ? html_dom_query(root, ".frvr-input-group") : 0;
    if (!group) return;
    HtmlElem old;
    while ((old = html_dom_query(group, ".input-error"))) html_dom_remove(old);
    char html[160];
    snprintf(html, sizeof(html), "<div class=\"input-error\">%s</div>", message);
    html_dom_insert_html(group, HTML_INSERT_BEFORE_END, html);
}

// v10 submitGameCode(): an empty field does nothing; a password that does not fit the JOIN packet is refused here
// (the packet would cut it short and the server would answer "Wrong server password")
static void server_password_submit(void) {
    char code[64] = "";
    const HtmlElem input = html_dom_by_id("codeInput");
    if (input) html_dom_get_value(input, code, sizeof(code));
    if (!code[0]) return;
    if (strlen(code) >= sizeof(((Client *) 0)->net_join.password)) {
        server_password_error("Password too long");
        return;
    }
    if (strike_screen_top() == STRIKE_SCREEN_SERVER_PASSWORD) strike_screen_pop(); // clearPops, then the join
    servers_ui_join_password(code);
}

// v10 knGameCodeLeave: location = location.origin, i.e. back to the menu (and a new seek where the menu has one)
static void server_password_leave(void) {
    if (strike_screen_top() == STRIKE_SCREEN_SERVER_PASSWORD) strike_screen_pop();
    servers_ui_retry();
}

// ---------------------------------------------------------------- FEEDBACK (v5 / g5)

// The main menu's feedback banner opens FRVR's form: the text (at least 5 characters, else the error line
// under it) and an optional e-mail (a valid address when one is typed). Submit posts /v1/feedback through the
// Strike gateway (strike_econ_request -> kn_brain's /strike/v1/feedback, services/brain/strike_econ.c); the
// thanks screen shows only on a 2xx answer, else the form stays with an error line under its buttons.
static struct { char text[512], email[160]; int validate, validate_email, sent, pending; } FB;
static unsigned g_form_serial; // one per opened form: an answer that comes back for an older form is dropped

static int email_valid(const char *e) {
    const char *at = strchr(e, '@');
    if (!at || at == e || strchr(e, ' ')) return 0;
    const char *dot = strrchr(at, '.');
    return dot && dot > at + 1 && dot[1] != 0;
}

// the error line under field 0 (the textarea) or 1 (the e-mail), or none (msg NULL)
static void feedback_error(const HtmlElem root, const int field, const char *msg) {
    HtmlElem groups[2];
    if (html_dom_query_all(root, ".feedback-form .frvr-input-group", groups, 2) < 2) return;
    const HtmlElem old = html_dom_query(groups[field], ".input-error");
    if (old) html_dom_remove(old);
    const HtmlElem control = html_dom_query(groups[field], field ? "input" : "textarea");
    if (control) html_dom_set_class(control, "is-error", msg != NULL);
    if (msg) {
        char html[160];
        snprintf(html, sizeof(html), "<div class=\"input-error\">%s</div>", msg);
        html_dom_insert_html(groups[field], HTML_INSERT_BEFORE_END, html);
    }
}

// validateExtraInfo / validateEmail: once Submit was pressed both are checked on every change
static void feedback_check(const HtmlElem root) {
    const int text_bad = FB.validate && strlen(FB.text) < 5;
    const int email_bad = FB.validate_email && FB.email[0] && !email_valid(FB.email);
    feedback_error(root, 0, text_bad ? "More information required (5 or more characters)." : NULL);
    feedback_error(root, 1, email_bad ? "Must be a valid e-mail." : NULL);
}

// the textarea / e-mail's oninput (strikeFeedback('text' | 'email')); a script's call may give the value as its
// second argument (call strikeFeedback "text" "...") in place of the typed one
static int h_strike_feedback(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user;
    if (argc < 1 || args[0].kind != HTML_ARG_STR || !args[0].str) return 1;
    const int scripted = argc > 1 && args[1].kind == HTML_ARG_STR && args[1].str;
    const char *v = scripted ? args[1].str : ev && ev->value ? ev->value : "";
    if (!strcmp(args[0].str, "text")) snprintf(FB.text, sizeof(FB.text), "%s", v);
    else if (!strcmp(args[0].str, "email")) snprintf(FB.email, sizeof(FB.email), "%s", v);
    else return 1;
    const HtmlElem root = strike_screen_root(STRIKE_SCREEN_FEEDBACK_FORM);
    if (root) feedback_check(root);
    return 0;
}

// the error line under the buttons (msg NULL: none); a new one replaces the old
static void feedback_submit_error(const HtmlElem root, const char *msg) {
    const HtmlElem old = html_dom_query(root, ".feedback-submit-error");
    if (old) html_dom_remove(old);
    const HtmlElem buttons = html_dom_query(root, ".feedback-form__buttons");
    if (!buttons || !msg) return;
    char html[256];
    snprintf(html, sizeof(html), "<div class=\"input-error feedback-submit-error\">%s</div>", msg);
    html_dom_insert_html(buttons, HTML_INSERT_BEFORE_BEGIN, html);
}

// the gateway's answer to the form's POST: 2xx = the feedback is stored, the thanks screen replaces the form
// (kr(g5): pop, push); anything else keeps the form open with the error line
static void feedback_answer(void *user, int status, const cJSON *data, const cJSON *error) {
    (void) data;
    if ((unsigned) (uintptr_t) user != g_form_serial) return; // that form was closed (or another one opened)
    FB.pending = 0;
    const HtmlElem root = strike_screen_root(STRIKE_SCREEN_FEEDBACK_FORM);
    if (!root) return;
    if (status >= 200 && status < 300) {
        FB.sent = 1;
        printf("[strike] feedback sent (%d)\n", status);
        strike_screen_pop();
        strike_screen_push(STRIKE_SCREEN_FEEDBACK_THANKS);
        return;
    }
    const char *server = cJSON_GetStringValue(cJSON_GetObjectItem(error, "message"));
    feedback_submit_error(root, status == 401 ? "You must be signed in to send feedback."
                                : server && status == 400 ? server
                                : "Your feedback could not be sent. Please try again.");
}

// Submit (the form's onclick, strikeAction('feedback-submit')): validate, then POST /v1/feedback {text, email}
// (FRVR's body; the e-mail only when typed). Nothing is sent while the text or the e-mail is invalid.
static void feedback_submit(void) {
    const HtmlElem root = strike_screen_root(STRIKE_SCREEN_FEEDBACK_FORM);
    if (!root || FB.sent || FB.pending) return;
    FB.validate = 1;
    FB.validate_email = FB.email[0] != 0; // shouldValidateEmail = !!this.email
    char *e = FB.email;
    while (*e == ' ') memmove(e, e + 1, strlen(e));    // email.trim()
    while (strlen(e) && e[strlen(e) - 1] == ' ') e[strlen(e) - 1] = 0;
    feedback_check(root);
    if (strlen(FB.text) < 5 || (FB.email[0] && !email_valid(FB.email))) return;
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "text", FB.text);
    if (FB.email[0]) cJSON_AddStringToObject(body, "email", FB.email);
    feedback_submit_error(root, NULL);
    FB.pending = 1;
    const int sent = strike_econ_request("POST", "/v1/feedback", body, feedback_answer, (void *) (uintptr_t) g_form_serial);
    cJSON_Delete(body);
    if (!sent) { // no Strike brain (offline, or not signed in to one): the form stays
        FB.pending = 0;
        printf("[strike] feedback not sent: no brain answers\n");
        feedback_submit_error(root, "Your feedback could not be sent. Please try again.");
    }
}

// ---------------------------------------------------------------- actions

// PLAY with no connection: the reconnect runs behind "Reconnecting..." (strike_connect.c: its deadline ends it in the
// Lost Connection popup, with Reconnect) and the game is entered once the reconnect lands. Connected, the map not in
// yet: the loader waits for it, and a game that does not start within PLAY_WAIT_MS is an error with Retry (never the
// loader for good). Entering the game takes the menu and its loader down, so a late timer does nothing.
#define PLAY_WAIT_MS 30000
static int g_play_timer;

static void play(void);
static void play_retry(void) { play(); }

static void play_expired(void *user) {
    (void) user;
    g_play_timer = 0;
    if (!strike_loading_shown() || !html_ui_is_menu_open()) return;
    printf("[strike] play: no game in %d s\n", PLAY_WAIT_MS / 1000);
    strike_loading_complete();
    strike_popup_retry(I18N("kn.strike.playFailed", "Couldn't join the game. Try again."), play_retry);
}

static void play(void) {
    if (!menu_connected()) {
        printf("[strike] play: not connected, reconnecting\n");
        strike_connection_state(STRIKE_CONN_RECONNECTING, NULL, NULL, 0, NULL); // "Reconnecting..." (strike_connect.c)
        menu_play_when_ready();
        return;
    }
    // quickConnect: Fm(Al, {info: "Loading <game> ..."}), then the game (RA when it starts)
    strike_loading_show("Loading  ...", 0);
    if (menu_can_play()) {
        menu_play();
        return;
    }
    // connected, the map not in yet (INIT still to come): entered once it is
    html_timer_cancel(g_play_timer);
    g_play_timer = html_timer_add(PLAY_WAIT_MS, play_expired, NULL);
    menu_play_when_ready();
}

// ---------------------------------------------------------------- Switch Teams (FRVR switchTeamsBtn)

// TAP_TO_PLAY's button above Play, in a team mode with selTeam (FRVR: Game.config.selTeam). A click
// starts a 5 s count, "Switching in N"; when it ends the other team is sent (FRVR's toggleTeam: SWAP_TEAM),
// then "Switch again in N" counts 10 s. The button is disabled meanwhile. The rule itself is the
// server's (mode_swap_team, shared/src/modes/mode_actions.c): the same modes, between lives only.
#define SWAP_IN_S 5
#define SWAP_AGAIN_S 10
#define SWAP_SEND_DELAY_S 0.5 // FRVR's setTimeout runs the toggle 500 ms after its count
enum { SWAP_IDLE, SWAP_IN, SWAP_AGAIN };
#define SWITCH_TEAMS_HTML                                                                                              \
    "<div id=\"switchTeamsBtn\" class=\"frvr-button frvr-button--gray frvr-button--size-xs frvr-button--width-150\" "  \
    "trackingid=\"switch_teams\" style=\"display: none;\" onclick=\"strikeAction('switch_teams')\">"                  \
    "<img src=\"./img/rotating-arrows.svg\" class=\"frvr-button__icon-left\">"                                         \
    "<span class=\"frvr-button-text\">Switch Teams</span></div>"

static int switch_teams_allowed(const Client *client) {
    if (!client || !client->me) return 0;
    const GameModeConfig *cfg = client->game.mode ? &client->game.mode->config : NULL;
    if (!cfg || !cfg->teams || cfg->friendly || cfg->clan_war || cfg->convert_team) return 0; // /team's modes
    return client->game.config.select_team && !client->game.config.comp; // comp rosters pick their teams
}

// NET_EXT_KN_SWAP_TEAM: the other team than the player's (FRVR: toggleTeam(team == 1 ? 2 : 1)).
static void send_swap_team(Client *client, const int team) {
    NetSwapTeamPacket req = {.team = (uint8_t) team};
    uint8_t buf[NET_EXT_HEADER_SIZE + sizeof(req)];
    const uint16_t n = net_ext_pack(buf, sizeof(buf), NET_EXT_KN_SWAP_TEAM, &req, sizeof(req));
    if (n && client) client_net_send(client, (NetPacketType) NET_PACKET_EXT, buf, n);
}

static void switch_teams_start(void) {
    if (!M.client || M.swap_phase != SWAP_IDLE || !switch_teams_allowed(M.client) || M.client->me->active) return;
    M.swap_phase = SWAP_IN;
    M.swap_t0 = gltf_clock();
}

// Each frame (strike_menu_tick): the count's end sends the switch, or drops it when the player is alive
// (a living player keeps its team, and the server refuses it too).
static void switch_teams_tick(Client *client) {
    if (M.swap_phase == SWAP_IDLE) return;
    const double now = gltf_clock();
    if (M.swap_phase == SWAP_IN && now - M.swap_t0 >= SWAP_IN_S + SWAP_SEND_DELAY_S) {
        if (client && client->me && !client->me->active) {
            send_swap_team(client, client->me->team == 1 ? 2 : 1);
            M.swap_phase = SWAP_AGAIN;
            M.swap_t0 = now;
        } else {
            M.swap_phase = SWAP_IDLE;
        }
    } else if (M.swap_phase == SWAP_AGAIN && now - M.swap_t0 >= SWAP_AGAIN_S + SWAP_SEND_DELAY_S) {
        M.swap_phase = SWAP_IDLE;
    }
}

// The button's label and enabled state; FRVR counts from total + 1 (its Yo / Xo start there).
static void switch_teams_update(const HtmlElem root, const Client *client) {
    HtmlElem btn = html_dom_query(root, "#switchTeamsBtn");
    if (!btn) {
        const HtmlElem join = html_dom_query(root, ".frvr-kr-limit");
        const HtmlElem bottom = html_dom_query(root, ".frvr-main-menu--bottom-right");
        if (join) html_dom_insert_html(join, HTML_INSERT_BEFORE_BEGIN, SWITCH_TEAMS_HTML);
        else if (bottom) html_dom_insert_html(bottom, HTML_INSERT_BEFORE_END, SWITCH_TEAMS_HTML);
        btn = html_dom_query(root, "#switchTeamsBtn");
        if (!btn) return;
        M.swap_shown[0] = 0; // a new button: write its state even when the key matches
    }
    const int allowed = switch_teams_allowed(client);
    char label[32] = "Switch Teams";
    const double now = gltf_clock();
    if (M.swap_phase == SWAP_IN) {
        const int left = SWAP_IN_S + 1 - (int) (now - M.swap_t0);
        snprintf(label, sizeof(label), "Switching in %d", left > 0 ? left : 0);
    } else if (M.swap_phase == SWAP_AGAIN) {
        const int left = SWAP_AGAIN_S + 1 - (int) (now - M.swap_t0);
        snprintf(label, sizeof(label), "Switch again in %d", left > 0 ? left : 0);
    }
    char key[64];
    snprintf(key, sizeof(key), "%d|%s", allowed, label);
    if (!strcmp(key, M.swap_shown)) return; // only write what changed
    snprintf(M.swap_shown, sizeof(M.swap_shown), "%s", key);
    const int counting = M.swap_phase != SWAP_IDLE;
    html_dom_set_display(btn, allowed ? "" : "none");
    if (counting) html_dom_set_attr(btn, "disabled", "");
    else html_dom_remove_attr(btn, "disabled");
    html_dom_set_style(btn, "opacity", counting ? "0.6" : "");
    const HtmlElem text = html_dom_query(btn, ".frvr-button-text");
    if (text) html_dom_set_text(text, label);
    const HtmlElem icon = html_dom_query(btn, "img");
    if (icon) html_dom_set_display(icon, counting ? "none" : "");
}

// quickConnect (FRVR): the boot gate's first run and the PLAY button
void strike_menu_quick_connect(void) { play(); }

static void action(const char *a) {
    printf("[strike] action %s\n", a);
    if (strike_notify_action(a)) return; // the notifications holder (strike_notify.c)
    if (strike_meta_action(a)) return;   // missions tab, match details report, rewards track (strike_meta.c)
    if (strike_social_action(a)) return; // friends, invites, report (strike_social.c)
    if (strike_reward_action(a)) return; // the reward screen, the choice popup (strike_reward.c)
    if (strike_connect_action(a)) return; // the Lost Connection popup's Reconnect (strike_connect.c)
    if (!strcmp(a, "join-password")) {   // SERVER_PASSWORD's Submit / Leave
        server_password_submit();
        return;
    }
    if (!strcmp(a, "join-password-leave")) {
        server_password_leave();
        return;
    }
    if (!strcmp(a, "ftue-tap")) strike_ftue_controls_tap(); // the controls tutorial's tap blur (strike_ftue_controls.c)
    else if (!strcmp(a, "ftue-go")) {
        // "Let's Go!" (FRVR: Tx, then backToOrigin): the tutorial ends and the game is left for the menu
        if (strike_ftue_controls_running()) {
            strike_ftue_controls_go();
            if (!html_ui_is_menu_open() && M.client) client_set_menu_open(M.client, 1);
            strike_screen_replace(STRIKE_SCREEN_MAIN_MENU);
        }
    } else if (!strcmp(a, "play")) play();
    else if (!strcmp(a, "switch_teams")) switch_teams_start();
    else if (!strncmp(a, "buy-confirm:", 12)) {
        // U2's YA: onPurchase (kt: the popup goes), then $r
        if (strike_screen_top() == STRIKE_SCREEN_POPUP) strike_screen_pop();
        purchase(atoi(a + 12));
    } else if (!strcmp(a, "spin-ticket")) {
        // the reward screen's Use Now!: kr(SA, {spinnerIndex: luckyDraw}): the spin-ticket wheel
        wheels_build();
        int i = -1;
        for (int k = 0; k < M.wheel_count && i < 0; k++) {
            if (M.wheels[k].ticket) i = k;
        }
        if (i >= 0) {
            M.spin = i;
            if (strike_screen_shown(STRIKE_SCREEN_SPIN)) {
                while (strike_screen_top() != STRIKE_SCREEN_SPIN) strike_screen_pop();
                strike_screen_rerender(STRIKE_SCREEN_SPIN);
            } else {
                strike_screen_push(STRIKE_SCREEN_SPIN);
            }
        }
    }
    else if (!strcmp(a, "double-xp")) strike_double_xp_claim(); // the lean end screen's Double XP (strike_double_xp.c)
    else if (!strcmp(a, "back")) {
        strike_screen_pop(); // kt()
        if (strike_screen_top() == STRIKE_SCREEN_NONE && html_ui_is_menu_open()) strike_screen_push(STRIKE_SCREEN_MAIN_MENU);
    } else if (!strncmp(a, "pop:", 4)) {
        // a close button that is onHide() + kt(): SPIN's home from the store (kt() twice), Customize
        // Controls' home (its onHide kt() and its own: Settings goes too)
        for (int i = atoi(a + 4); i > 0; i--) strike_screen_pop();
        if (strike_screen_top() == STRIKE_SCREEN_NONE && html_ui_is_menu_open()) strike_screen_push(STRIKE_SCREEN_MAIN_MENU);
    } else if (!strcmp(a, "popup:buy-tickets")) {
        tickets_open(); // Jce (a capture / scenario opens the exchange directly)
    } else if (!strncmp(a, "ticket-step:", 12)) {
        M.ticket_n += atoi(a + 12); // Sce(Mf + step): tickets_fill keeps it within [1, max]
        strike_screen_rerender(STRIKE_SCREEN_POPUP);
    } else if (!strcmp(a, "ticket-buy")) {
        buy_tickets();
    } else if (!strncmp(a, "spin:", 5)) {
        const int i = atoi(a + 5);
        wheels_build();
        if (i >= 0 && i < M.wheel_count) {
            M.spin = i;
            strike_screen_push(STRIKE_SCREEN_SPIN); // eb(id, {onHide: kt}) = Pt(SA, {spinnerIndex})
        }
    } else if (!strcmp(a, "spin-golden")) {
        // the golden block's button (FRVR luckyDrawOneSpin): the golden wheel's screen
        if (golden_wheel()) {
            M.spin = GOLDEN_SPIN;
            strike_screen_push(STRIKE_SCREEN_SPIN);
        }
    } else if (!strncmp(a, "item-slot:", 10)) {
        const int cat = atoi(a + 10);
        if (cat >= 0 && cat < CAT_COUNT) {
            M.item_cat = cat;
            // T5's default thumbnail: the box's own default image
            M.item_default_img[0] = 0;
            const HtmlElem root = strike_screen_root(STRIKE_SCREEN_CUSTOMIZATION);
            HtmlElem boxes[24];
            const int n = root ? html_dom_query_all(root, ".customization-screen__cat-box", boxes, 24) : 0;
            for (int i = 0; i < n; i++) {
                const HtmlElem t = html_dom_query(boxes[i], ".customization-screen__cat-box-title span");
                char title[48] = "";
                if (t) html_dom_get_text(t, title, sizeof(title));
                const HtmlElem img = html_dom_query(boxes[i], ".customization-screen__cat-box-middle img");
                if (img && !strcmp(title, CATS[cat].title)) html_dom_get_attr(img, "data-kn-default", M.item_default_img, sizeof(M.item_default_img));
            }
            strike_screen_push(STRIKE_SCREEN_ITEM_SELECTION);
        }
    } else if (!strncmp(a, "item-pick:", 10)) {
        item_pick(atoi(a + 10));
    } else if (!strncmp(a, "leaderboard", 11)) {
        const char *board = a[11] == ':' ? a + 12 : "xp";
        if (!strike_econ_get_leaderboard(board, BOARD_MAX, 0, on_board, NULL)) printf("[strike] leaderboard %s: no kn_brain\n", board);
    } else if (!strncmp(a, "spin-buy:", 9)) {
        spin_buy(atoi(a + 9) == 10 ? 10 : 1);
    } else if (!strncmp(a, "buy-product:", 12)) {
        buy_product(atoi(a + 12));
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
    else if (!strcmp(a, "banner-ad")) strike_banner_ad_claim(); // the round end's banner slot (strike_banner_ad.c)
    else if (!strncmp(a, "class:", 6)) {
        const int i = atoi(a + 6);
        if (i >= 0 && i < STRIKE_CLASS_COUNT) {
            M.previewed = i;
            const HtmlElem root = strike_screen_root(STRIKE_SCREEN_CLASS_SELECTION);
            if (root) class_selection_fill(root);
        }
    } else if (!strcmp(a, "select-class")) {
        if (M.previewed != current_class() && class_owned(M.previewed)) {
            menu_set_class_index(CLASSES[M.previewed].id, 0); // window.selectClass(_r)
            strike_screen_pop();                               // kt()
        }
    } else if (!strncmp(a, "settings-tab:", 13)) {
        const int t = atoi(a + 13);
        const HtmlElem root = strike_screen_root(STRIKE_SCREEN_SETTINGS);
        if (root && t >= 0 && t < (int) (sizeof(SETTINGS_TABS) / sizeof(SETTINGS_TABS[0]))) settings_tab(root, t);
    } else if (!strcmp(a, "fullscreen")) {
        // the Display tab's Fullscreen Mode button: F11's toggle, then the row's text follows the window
        client_render_toggle_fullscreen(M.client);
        const HtmlElem root = strike_screen_root(STRIKE_SCREEN_SETTINGS);
        if (root) settings_tab(root, M.settings_tab);
    } else if (!strcmp(a, "dmgnum-toggle")) {
        g_dmg_open = !g_dmg_open;
        damage_row_refresh();
    } else if (!strncmp(a, "dmgnum:", 7)) {
        const int mode = atoi(a + 7);
        if (mode >= 0 && mode < DMG_MODE_COUNT) {
            HtmlArg v = {0};
            v.kind = HTML_ARG_NUM;
            v.num = mode;
            ui_settings_set("damageNumberDisplayMode", &v);
        }
        g_dmg_open = 0;
        damage_row_refresh();
    } else if (!strncmp(a, "preset:", 7)) {
        const int button = atoi(a + 7);
        if (button >= 0 && button < 4) preset_select(BUTTON_PRESET[button]);
        const HtmlElem root = strike_screen_root(STRIKE_SCREEN_SETTINGS);
        if (root) settings_tab(root, 0);
    } else if (!strcmp(a, "unlock-info")) {
        class_unlock_open(); // cde(): the store or the profile
    } else if (!strcmp(a, "feedback")) {
        strike_screen_push(STRIKE_SCREEN_FEEDBACK_FORM); // Pt(v5)
    } else if (!strcmp(a, "feedback-submit")) {
        feedback_submit();
    } else if (!strcmp(a, "kr-limit-info")) {
        kr_bubble_toggle();
    } else if (!strcmp(a, "kr-limit-bubble-close")) {
        kr_bubble_hide();
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
    case STRIKE_SCREEN_POPUP_WARNING: // the connection popups (strike_connect.c)
    case STRIKE_SCREEN_MATCH_DETAILS:   // transcribed from the bundle (strike_meta.c)
    case STRIKE_SCREEN_REWARD_TO_CLAIM: // transcribed from the bundle (strike_meta.c)
    case STRIKE_SCREEN_FEEDBACK_FORM:   // transcribed from the bundle (strike_menu.c, feedback)
    case STRIKE_SCREEN_FEEDBACK_THANKS:
    case STRIKE_SCREEN_ITEM_SELECTION:  // transcribed from the bundle (T5 / rde, above)
    case STRIKE_SCREEN_REPORT_PLAYER:   // transcribed from the bundle (strike_social.c)
    case STRIKE_SCREEN_PROFILE_DETAILS: // jC (strike_profile.c)
    case STRIKE_SCREEN_POPUP_MESSAGE:   // Oue (strike_profile.c)
    case STRIKE_SCREEN_SERVER_PASSWORD: // KrunkNative's own prompt (SERVER_PASSWORD.html)
        return strike_screen_id(s);
    case STRIKE_SCREEN_REWARD:          // transcribed from the bundle (strike_reward.c)
    case STRIKE_SCREEN_REWARD_CHOICE:   // transcribed from the bundle (strike_reward.c)
    case STRIKE_SCREEN_INPUT_BLOCKER:   // transcribed from the bundle (strike_reward.c)
        return strike_screen_id(s);
    case STRIKE_SCREEN_POPUP_SUCCESS:   // Oue, the alert both use (strike_social.c)
    case STRIKE_SCREEN_POPUP_ERROR:
        return "POPUP_ALERT";
    default:
        return NULL; // no template of it yet (README.md "Not ported yet"); MISSION_TAB_SCREEN is
                     // the Battle Pass screen's Missions tab (strikeAction('missions'))
    }
}

// strike_econ_listen: Mithril redraws every view on a state change; here the views that show the
// account's Strike state follow it (wallet, items, loadout, progress)
static void on_econ(void *user, const StrikeEconEvent event) {
    (void) user;
    (void) event;
    M.last_account[0] = 0; // the account box (strike_menu_tick)
    HtmlElem root;
    if ((root = strike_screen_root(STRIKE_SCREEN_STORE))) {
        fill_currency(root);
        store_spins_fill(root); // the golden block's meter and its ticket / purchase button
    }
    if ((root = strike_screen_root(STRIKE_SCREEN_SPIN))) fill_currency(root);
    if (M.popup == POPUP_BUY_TICKETS && strike_screen_shown(STRIKE_SCREEN_POPUP)) strike_screen_rerender(STRIKE_SCREEN_POPUP);
    if ((root = strike_screen_root(STRIKE_SCREEN_CUSTOMIZATION))) customization_fill(root);
    if ((root = strike_screen_root(STRIKE_SCREEN_ITEM_SELECTION))) item_selection_fill(root);
    strike_meta_econ_changed(); // missions, the rewards track (strike_meta.c)
}

void strike_menu_install(void) {
    static int listening;
    if (!listening) listening = strike_econ_listen(on_econ, NULL);
    preset_load();
    html_ui_add_handler("strikeAction", h_strike_action);
    html_ui_add_handler("strikeFeedback", h_strike_feedback);
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

// TAP_TO_PLAY's top-centre mode label (FRVR's Yw, .frvr-tap-to-play-map-mode-name-label): the game
// mode's name, as the end screen's header has it (strike_hud.c). The template is FRVR's sandbox
// capture, which had no mode and wrote the text "null" there; with no mode the label is empty.
static void tap_to_play_mode_label(const HtmlElem root, const Client *client) {
    const char *mode = client && client->game.mode && client->game.mode->config.name ? client->game.mode->config.name : "";
    set_text(root, ".frvr-tap-to-play-map-mode-name-label", mode);
}

void strike_menu_render(const StrikeScreen s, const HtmlElem root) {
    switch (s) {
    case STRIKE_SCREEN_MAIN_MENU:
        class_preview_canvas(root, "canvas", 0.0f, 0.0f, 100.0f, 100.0f); // MAIN_MENU: canvas{width:100%;height:100%} of the screen
        fill_account(root);
        kr_limit_hook(root);
        break;
    case STRIKE_SCREEN_TAP_TO_PLAY:
        fill_account(root);
        kr_limit_hook(root);
        switch_teams_update(root, M.client);
        tap_to_play_mode_label(root, M.client);
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
        else presets_mark(root);
        break;
    case STRIKE_SCREEN_ITEM_SELECTION:
        item_selection_fill(root);
        break;
    case STRIKE_SCREEN_CUSTOMIZATION: {
        customization_fill(root);
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
    case STRIKE_SCREEN_POPUP_WARNING:
        strike_connect_render(root);
        break;
    case STRIKE_SCREEN_SERVER_PASSWORD: {
        const HtmlElem input = html_dom_by_id("codeInput"); // v10 gameCode(): the field has the focus
        if (input) html_dom_focus(input);
        break;
    }
    case STRIKE_SCREEN_FEEDBACK_FORM:
        memset(&FB, 0, sizeof(FB)); // a new form: empty, nothing validated
        g_form_serial++;
        break;
    default:
        break;
    }
    strike_meta_render(s, root);   // the Battle Pass tabs, the Rewards Track, match details, reward card
    strike_social_render(s, root); // the friends strip / list, Private Match's invites, report, alerts
    strike_reward_render(s, root); // the reward screen, the choice popup, the input blocker
}

const char *strike_menu_class_for_weapon(const int weapon) {
    const ClassConfig *c = rules_classes();
    for (int i = 0; c && i < STRIKE_CLASS_COUNT; i++) {
        const int id = CLASSES[i].id;
        if (id >= 0 && id < (int) rules_class_count() && c[id].loadout && c[id].loadout[0] == weapon) return CLASSES[i].name;
    }
    return NULL;
}

// state.strike of a --render-scenario (sample data):
//   itemSelection: {category: CATS index, owned: [item ids], equipped: {slot: id}, skinColor} over a
//                  CUSTOMIZATION_SCREEN scenario: the box's click (T5) with these items
//   product:       a /strike/v1/shop product: the market card's click (U2 in the POPUP frame)
static void scenario_items(const cJSON *v);

void strike_menu_scenario_state(const cJSON *st) {
    scenario_items(cJSON_GetObjectItemCaseSensitive(st, "itemSelection"));
    const cJSON *product = cJSON_GetObjectItemCaseSensitive(st, "product");
    if (cJSON_IsObject(product)) {
        cJSON_Delete(M.shop);
        M.shop = cJSON_CreateObject();
        cJSON_AddItemToArray(cJSON_AddArrayToObject(M.shop, "products"), cJSON_Duplicate(product, 1));
        buy_product(0);
    }
}

static void scenario_items(const cJSON *v) {
    if (!cJSON_IsObject(v)) return;
    cJSON_Delete(ITEMS.owned);
    cJSON_Delete(ITEMS.equipped);
    ITEMS.owned = cJSON_CreateArray();
    const cJSON *o;
    cJSON_ArrayForEach(o, cJSON_GetObjectItemCaseSensitive(v, "owned")) {
        if (!cJSON_IsString(o)) continue;
        cJSON *row = cJSON_CreateObject();
        cJSON_AddStringToObject(row, "itemId", o->valuestring);
        cJSON_AddItemToArray(ITEMS.owned, row);
    }
    const cJSON *eq = cJSON_GetObjectItemCaseSensitive(v, "equipped");
    ITEMS.equipped = cJSON_IsObject(eq) ? cJSON_Duplicate(eq, 1) : cJSON_CreateObject();
    ITEMS.set = 1;
    const cJSON *sc = cJSON_GetObjectItemCaseSensitive(v, "skinColor");
    if (cJSON_IsNumber(sc)) ui_loadout_select_skin_color(sc->valueint);
    char a[32];
    snprintf(a, sizeof(a), "item-slot:%d", (int) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(v, "category")));
    action(a);
}

int strike_menu_class_id(void) { return CLASSES[current_class()].id; }

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
    switch_teams_tick(client);
    strike_social_tick(client);
    strike_reward_tick(client);
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
    const HtmlElem tap = strike_screen_root(STRIKE_SCREEN_TAP_TO_PLAY);
    if (tap) switch_teams_update(tap, client); // the button follows the mode, the count and the life
}

// ---------------------------------------------------------------- the controller in the screens
// FRVR 1.37.2's pages take the mouse and the touch only. KrunkNative's controller works them as a
// focus ring while the menu is open on Krunker Strike (gamepad.c routes the pad here): the d-pad or
// the left stick moves the focus to the nearest visible button in that direction, A (button_1)
// clicks it through the page's own mouse path, B (button_2) sends Escape (the page's back / close).
// START keeps opening and closing the menu (gamepad.c). The game's controls are untouched: this only
// runs while the menu is open and the loading screen is not up.
#include <GLFW/glfw3.h>

static struct {
    HtmlElem focus;   // the outlined button (0: none)
} P;

#define PAD_MAX 256
#define PAD_SELECTOR "button, [onclick], .frvr-button"

// The focus ring's outline (inline style, so the page's CSS needs nothing).
static void pad_outline(const HtmlElem el, const int on) {
    if (!el) return;
    html_dom_set_style(el, "outline", on ? "3px solid #ffd54f" : "");
    html_dom_set_style(el, "outline-offset", on ? "2px" : "");
}

static int pad_active(void) {
    return strike_ui_active() && html_ui_is_menu_open() && !strike_loading_up();
}

// The ring's element while it is still one of the page's buttons (a screen change may have removed it:
// the handle is only dereferenced once it is in the page's current list). 0 otherwise.
static HtmlElem pad_live_focus(void) {
    if (!P.focus) return 0;
    static HtmlElem all[PAD_MAX];
    const int n = html_dom_query_all(0, PAD_SELECTOR, all, PAD_MAX);
    for (int i = 0; i < n && i < PAD_MAX; i++) {
        if (all[i] == P.focus) return P.focus;
    }
    P.focus = 0;
    return 0;
}

// A visible element with a box in the viewport, as its centre in CSS px.
static int pad_centre(const HtmlElem el, float *cx, float *cy, float vw, float vh) {
    HtmlRect r;
    if (!el || !html_dom_is_shown(el) || !html_dom_get_rect(el, &r) || r.w <= 0.0f || r.h <= 0.0f) return 0;
    *cx = r.x + r.w / 2.0f;
    *cy = r.y + r.h / 2.0f;
    return *cx >= 0.0f && *cy >= 0.0f && *cx <= vw && *cy <= vh;
}

// The page's CSS px and the framebuffer px the page's mouse takes (the window is the framebuffer).
static void pad_click(const float cx, const float cy, const float dpr) {
    html_ui_mouse_move((double) cx * dpr, (double) cy * dpr);
    html_ui_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
    html_ui_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE, 0);
}

// Moves the focus one step in (dx, dy) (+x right, +y down): the nearest visible button that lies in that
// direction, scored by its distance along it plus twice its offset across it; with no focus, the first one.
static void pad_move(const int dx, const int dy) {
    float vw = 0.0f, vh = 0.0f, dpr = 1.0f;
    html_dom_viewport(&vw, &vh, &dpr);
    static HtmlElem all[PAD_MAX];
    const HtmlElem focus = pad_live_focus();
    const int n = html_dom_query_all(0, PAD_SELECTOR, all, PAD_MAX);
    float fx = 0.0f, fy = 0.0f;
    const int have = focus && pad_centre(focus, &fx, &fy, vw, vh);
    HtmlElem best = 0;
    float best_score = 1e30f;
    for (int i = 0; i < n && i < PAD_MAX; i++) {
        float cx = 0.0f, cy = 0.0f;
        if (!pad_centre(all[i], &cx, &cy, vw, vh) || all[i] == focus) continue;
        if (!have) { // the top-left-most one
            const float score = cy * 10000.0f + cx;
            if (score < best_score) best_score = score, best = all[i];
            continue;
        }
        const float along = (cx - fx) * dx + (cy - fy) * dy;
        if (along <= 1.0f) continue;
        const float across = fabsf((cx - fx) * dy) + fabsf((cy - fy) * dx);
        const float score = along + 2.0f * across;
        if (score < best_score) best_score = score, best = all[i];
    }
    if (!best) return; // no button that way: the ring stays where it is
    pad_outline(focus, 0);
    P.focus = best;
    pad_outline(P.focus, 1);
}

// The focus ring's A: a click at the focused button's centre.
static void pad_press(void) {
    float vw = 0.0f, vh = 0.0f, dpr = 1.0f;
    html_dom_viewport(&vw, &vh, &dpr);
    float cx = 0.0f, cy = 0.0f;
    const HtmlElem focus = pad_live_focus();
    if (!focus || !pad_centre(focus, &cx, &cy, vw, vh)) return;
    pad_click(cx, cy, dpr > 0.0f ? dpr : 1.0f);
}

// gamepad.c, on a button press while the menu is open: 1 = the focus ring took it (START is not the ring's).
int strike_menu_pad_button(const int button, const int pressed) {
    if (!pressed || !pad_active()) return 0;
    switch (button) {
        case 12: pad_move(0, -1); return 1; // d_pad_up
        case 13: pad_move(0, 1); return 1;  // d_pad_down
        case 14: pad_move(-1, 0); return 1; // d_pad_left
        case 15: pad_move(1, 0); return 1;  // d_pad_right
        case 0: pad_press(); return 1;      // button_1 (A)
        case 1: // button_2 (B): Escape, the page's back
            html_ui_key(GLFW_KEY_ESCAPE, 0, GLFW_PRESS, 0);
            html_ui_key(GLFW_KEY_ESCAPE, 0, GLFW_RELEASE, 0);
            return 1;
        default: return 0;
    }
}

// gamepad.c, the left stick's first deflection while the menu is open: one step of the ring in its
// dominant direction (x right, y down). 1 = taken.
int strike_menu_pad_stick(const double x, const double y) {
    if (!pad_active()) return 0;
    if (fabs(x) >= fabs(y)) pad_move(x > 0.0 ? 1 : -1, 0);
    else pad_move(0, y > 0.0 ? 1 : -1);
    return 1;
}

// The menu closed (or the screens went): the ring lets go of its element.
void strike_menu_pad_forget(void) {
    const HtmlElem focus = pad_live_focus();
    pad_outline(focus, 0);
    P.focus = 0;
}
