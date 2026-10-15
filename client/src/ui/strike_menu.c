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
// The Strike classes are FRVR's six (tools/strike/strike_data.json "classes"): Krunker class ids
// 0 Triggerman, 1 Hunter, 2 Run N Gun, 4 Shotgunner (Krunker's Vince), 5 Detective, 7 Rocketeer.
#include <strike_ui.h>
#include <html_ui.h>
#include <ui_logic.h>
#include <backend_ui.h>
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

static struct {
    Client *client;
    int previewed;        // _r: the class bullet shown in CLASS_SELECTION
    int settings_tab;
    int preset;           // selectedPresetIndex (3 = Custom)
    char last_account[160];
} M = {.preset = 3};

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

// The currency header of STORE / SPIN (FRVR's .currency-header): spin tickets and KR as the account box
// shows them (the tickets are 0 on a fresh account, as fill_account does)
static void fill_currency(const HtmlElem root) {
    char name[64], buf[32];
    int level = 1, kr = 0;
    account_values(name, sizeof(name), &level, &kr);
    HtmlElem spans[8];
    snprintf(buf, sizeof(buf), "%d", FIXTURE.set ? FIXTURE.tickets : 0);
    int n = html_dom_query_all(root, ".spin-tickets span", spans, 8);
    for (int i = 0; i < n; i++) html_dom_set_text(spans[i], buf);
    snprintf(buf, sizeof(buf), "%d", kr);
    n = html_dom_query_all(root, ".krunkies span", spans, 8);
    for (int i = 0; i < n; i++) html_dom_set_text(spans[i], buf);
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
    } else if (!strcmp(a, "home")) {
        if (!html_ui_is_menu_open() && M.client) client_set_menu_open(M.client, 1);
        strike_screen_replace(STRIKE_SCREEN_MAIN_MENU); // nu(yg)
    } else if (!strcmp(a, "settings")) strike_screen_push(STRIKE_SCREEN_SETTINGS);
    else if (!strcmp(a, "customize")) strike_screen_push(STRIKE_SCREEN_CUSTOMIZATION);
    else if (!strcmp(a, "class-selection")) strike_screen_push(STRIKE_SCREEN_CLASS_SELECTION);
    else if (!strcmp(a, "profile")) strike_screen_push(STRIKE_SCREEN_PROFILE);
    else if (!strcmp(a, "store")) strike_screen_push(STRIKE_SCREEN_STORE);
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
    case STRIKE_SCREEN_SPIN:
        fill_currency(root);
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
