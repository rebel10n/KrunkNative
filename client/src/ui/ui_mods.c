// mod/mods.ts of the Krunker offline builds (v3.1.1, unchanged in v7.1.4), the parts around the developer console
// (client/include/ui_mods.h). Rows are built with the same markup mods.ts creates and
// inserted where its ensure*Row() puts them; the switches are read back each frame (the
// mod's click listeners) and the bind rows react to the left button (ui_mods_pointer,
// natively from main.c, on the web through kn-shim's data-kn-pointer glue).
#include <ui_mods.h>
#include <console.h>
#include <app_mode.h> // the toggle key on the other pages
#include <ui_logic.h>
#include <html_ui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MODS_KEY_DEV_ENABLED "kro_setngss_devConsoleEnabled"
#define MODS_KEY_DEV_BIND "krk_dev_console_bind"
#define MODS_KEY_CYCLE_BIND "krk_cycle_command_bind"
#define MODS_KEY_FORCE_OFFLINE "krk_force_offline_sandbox"
#define MODS_CODE_MAX 32

void client_send_cycle_packet(Client *client); // main.c: io.send("cycle")

enum { CAPTURE_NONE, CAPTURE_DEV, CAPTURE_CYCLE, CAPTURE_BHOP };

static struct {
    int loaded;
    int dev_enabled;
    char dev_key[MODS_CODE_MAX];
    char cycle_key[MODS_CODE_MAX];
    int force_offline;
    int capturing;               // devConsoleCapturingKey / cycleCommandCapturingKey / bhopModeCapturingKey
    HtmlElem pressed;            // span under the left button (click = down + up on it)
    char shown[3][96];            // last text written into the three key spans
} g_mods;

// ---------------------------------------------------------------- stored values (readStored*)

static void mods_read_string(const char *key, char *out, const size_t n, const char *fallback) {
    if (html_storage_get(key, out, n) < 0) snprintf(out, n, "%s", fallback);
}

static int mods_read_bool(const char *key, const int fallback) {
    char buf[16];
    if (html_storage_get(key, buf, sizeof(buf)) < 0) return fallback;
    return !strcmp(buf, "true") || !strcmp(buf, "1");
}

static void mods_load(void) {
    if (g_mods.loaded) return;
    g_mods.loaded = 1;
    g_mods.dev_enabled = mods_read_bool(MODS_KEY_DEV_ENABLED, 1);
    mods_read_string(MODS_KEY_DEV_BIND, g_mods.dev_key, sizeof(g_mods.dev_key), "Backquote");
    mods_read_string(MODS_KEY_CYCLE_BIND, g_mods.cycle_key, sizeof(g_mods.cycle_key), "F8");
    g_mods.force_offline = mods_read_bool(MODS_KEY_FORCE_OFFLINE, 1);
}

void ui_mods_reload(void) {
    g_mods.loaded = 0;
    mods_load();
}

int ui_mods_dev_console_enabled(void) {
    mods_load();
    return g_mods.dev_enabled;
}

const char *ui_mods_dev_console_key(void) {
    mods_load();
    return g_mods.dev_key;
}

int ui_mods_force_offline(void) {
    mods_load();
    return g_mods.force_offline;
}

// ---------------------------------------------------------------- key codes

typedef struct { int glfw; const char *code; const char *key; const char *shift_key; } KeyRow;

static const KeyRow KEY_ROWS[] = {
    {GLFW_KEY_SPACE, "Space", " ", " "}, {GLFW_KEY_APOSTROPHE, "Quote", "'", "\""}, {GLFW_KEY_COMMA, "Comma", ",", "<"},
    {GLFW_KEY_MINUS, "Minus", "-", "_"}, {GLFW_KEY_PERIOD, "Period", ".", ">"}, {GLFW_KEY_SLASH, "Slash", "/", "?"},
    {GLFW_KEY_SEMICOLON, "Semicolon", ";", ":"}, {GLFW_KEY_EQUAL, "Equal", "=", "+"},
    {GLFW_KEY_LEFT_BRACKET, "BracketLeft", "[", "{"}, {GLFW_KEY_BACKSLASH, "Backslash", "\\", "|"},
    {GLFW_KEY_RIGHT_BRACKET, "BracketRight", "]", "}"}, {GLFW_KEY_GRAVE_ACCENT, "Backquote", "`", "~"},
    {GLFW_KEY_WORLD_1, "IntlBackslash", "\\", "|"}, {GLFW_KEY_ESCAPE, "Escape", "Escape", "Escape"},
    {GLFW_KEY_ENTER, "Enter", "Enter", "Enter"}, {GLFW_KEY_TAB, "Tab", "Tab", "Tab"},
    {GLFW_KEY_BACKSPACE, "Backspace", "Backspace", "Backspace"}, {GLFW_KEY_INSERT, "Insert", "Insert", "Insert"},
    {GLFW_KEY_DELETE, "Delete", "Delete", "Delete"}, {GLFW_KEY_RIGHT, "ArrowRight", "ArrowRight", "ArrowRight"},
    {GLFW_KEY_LEFT, "ArrowLeft", "ArrowLeft", "ArrowLeft"}, {GLFW_KEY_DOWN, "ArrowDown", "ArrowDown", "ArrowDown"},
    {GLFW_KEY_UP, "ArrowUp", "ArrowUp", "ArrowUp"}, {GLFW_KEY_PAGE_UP, "PageUp", "PageUp", "PageUp"},
    {GLFW_KEY_PAGE_DOWN, "PageDown", "PageDown", "PageDown"}, {GLFW_KEY_HOME, "Home", "Home", "Home"},
    {GLFW_KEY_END, "End", "End", "End"}, {GLFW_KEY_CAPS_LOCK, "CapsLock", "CapsLock", "CapsLock"},
    {GLFW_KEY_SCROLL_LOCK, "ScrollLock", "ScrollLock", "ScrollLock"}, {GLFW_KEY_NUM_LOCK, "NumLock", "NumLock", "NumLock"},
    {GLFW_KEY_PRINT_SCREEN, "PrintScreen", "PrintScreen", "PrintScreen"}, {GLFW_KEY_PAUSE, "Pause", "Pause", "Pause"},
    {GLFW_KEY_KP_DECIMAL, "NumpadDecimal", ".", "."}, {GLFW_KEY_KP_DIVIDE, "NumpadDivide", "/", "/"},
    {GLFW_KEY_KP_MULTIPLY, "NumpadMultiply", "*", "*"}, {GLFW_KEY_KP_SUBTRACT, "NumpadSubtract", "-", "-"},
    {GLFW_KEY_KP_ADD, "NumpadAdd", "+", "+"}, {GLFW_KEY_KP_ENTER, "NumpadEnter", "Enter", "Enter"},
    {GLFW_KEY_KP_EQUAL, "NumpadEqual", "=", "="}, {GLFW_KEY_LEFT_SHIFT, "ShiftLeft", "Shift", "Shift"},
    {GLFW_KEY_RIGHT_SHIFT, "ShiftRight", "Shift", "Shift"}, {GLFW_KEY_LEFT_CONTROL, "ControlLeft", "Control", "Control"},
    {GLFW_KEY_RIGHT_CONTROL, "ControlRight", "Control", "Control"}, {GLFW_KEY_LEFT_ALT, "AltLeft", "Alt", "Alt"},
    {GLFW_KEY_RIGHT_ALT, "AltRight", "Alt", "Alt"}, {GLFW_KEY_LEFT_SUPER, "MetaLeft", "Meta", "Meta"},
    {GLFW_KEY_RIGHT_SUPER, "MetaRight", "Meta", "Meta"}, {GLFW_KEY_MENU, "ContextMenu", "ContextMenu", "ContextMenu"},
};

static const char DIGIT_SHIFT[] = ")!@#$%^&*(";

const char *ui_mods_key_code(const int key) {
    static char buf[MODS_CODE_MAX];
    if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z) snprintf(buf, sizeof(buf), "Key%c", 'A' + (key - GLFW_KEY_A));
    else if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9) snprintf(buf, sizeof(buf), "Digit%c", '0' + (key - GLFW_KEY_0));
    else if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F24) snprintf(buf, sizeof(buf), "F%d", 1 + (key - GLFW_KEY_F1));
    else if (key >= GLFW_KEY_KP_0 && key <= GLFW_KEY_KP_9) snprintf(buf, sizeof(buf), "Numpad%c", '0' + (key - GLFW_KEY_KP_0));
    else {
        for (size_t i = 0; i < sizeof(KEY_ROWS) / sizeof(KEY_ROWS[0]); i++) {
            if (KEY_ROWS[i].glfw == key) return KEY_ROWS[i].code;
        }
        return "";
    }
    return buf;
}

const char *ui_mods_key_value(const int key, const int mods) {
    static char buf[MODS_CODE_MAX];
    const int shift = (mods & GLFW_MOD_SHIFT) != 0;
    if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z) snprintf(buf, sizeof(buf), "%c", (shift ? 'A' : 'a') + (key - GLFW_KEY_A));
    else if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9) snprintf(buf, sizeof(buf), "%c", shift ? DIGIT_SHIFT[key - GLFW_KEY_0] : '0' + (key - GLFW_KEY_0));
    else if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F24) snprintf(buf, sizeof(buf), "F%d", 1 + (key - GLFW_KEY_F1));
    else if (key >= GLFW_KEY_KP_0 && key <= GLFW_KEY_KP_9) snprintf(buf, sizeof(buf), "%c", '0' + (key - GLFW_KEY_KP_0));
    else {
        for (size_t i = 0; i < sizeof(KEY_ROWS) / sizeof(KEY_ROWS[0]); i++) {
            if (KEY_ROWS[i].glfw == key) return shift ? KEY_ROWS[i].shift_key : KEY_ROWS[i].key;
        }
        return "";
    }
    return buf;
}

int ui_mods_code_matches(const char *code, const int key) {
    return code && *code && !strcmp(code, ui_mods_key_code(key));
}

// formatKeyBind(code)
const char *ui_mods_format_key_bind(const char *code) {
    static char buf[48];
    if (!code || !*code) return I18N("kn.mods.unbound", "Unbound");
    static const char *const SPECIAL[][2] = {
        {"Backquote", "`"}, {"Minus", "-"}, {"Equal", "="}, {"BracketLeft", "["}, {"BracketRight", "]"},
        {"Backslash", "\\"}, {"Semicolon", ";"}, {"Quote", "'"}, {"Comma", ","}, {"Period", "."}, {"Slash", "/"},
        {"Space", "Space"}, {"Escape", "Esc"}, {"ArrowUp", "Up"}, {"ArrowDown", "Down"}, {"ArrowLeft", "Left"},
        {"ArrowRight", "Right"},
    };
    for (size_t i = 0; i < sizeof(SPECIAL) / sizeof(SPECIAL[0]); i++) {
        if (!strcmp(code, SPECIAL[i][0])) return SPECIAL[i][1];
    }
    if (!strncmp(code, "Key", 3) && code[3] >= 'A' && code[3] <= 'Z' && !code[4]) return code + 3;
    if (!strncmp(code, "Digit", 5) && code[5] >= '0' && code[5] <= '9' && !code[6]) return code + 5;
    if (!strncmp(code, "Numpad", 6) && code[6] >= '0' && code[6] <= '9' && !code[7]) {
        snprintf(buf, sizeof(buf), "Num %c", code[6]);
        return buf;
    }
    // .replace(/([a-z0-9])([A-Z])/g, "$1 $2").replace(/(Left|Right)$/, " $1")
    size_t o = 0;
    for (size_t i = 0; code[i] && o + 2 < sizeof(buf); i++) {
        const char c = code[i];
        if (i && c >= 'A' && c <= 'Z') {
            const char p = code[i - 1];
            if ((p >= 'a' && p <= 'z') || (p >= '0' && p <= '9')) buf[o++] = ' ';
        }
        buf[o++] = c;
    }
    buf[o] = 0;
    return buf;
}

// ---------------------------------------------------------------- setters (set*)

static void mods_refresh_ui(void);

static void mods_set_dev_enabled(const int value) {
    g_mods.dev_enabled = !!value;
    html_storage_set(MODS_KEY_DEV_ENABLED, g_mods.dev_enabled ? "true" : "false");
    if (!g_mods.dev_enabled && ui_console_is_open()) {
        menu_play();                 // instructionHolder.click()
        ui_console_toggle(0);
    }
    mods_refresh_ui();
}

static void mods_set_force_offline(const int value) {
    g_mods.force_offline = !!value;
    html_storage_set(MODS_KEY_FORCE_OFFLINE, g_mods.force_offline ? "true" : "false");
    mods_refresh_ui();
}

static void mods_set_bind(const int which, const char *code) {
    if (which == CAPTURE_DEV) {
        snprintf(g_mods.dev_key, sizeof(g_mods.dev_key), "%s", code ? code : "");
        html_storage_set(MODS_KEY_DEV_BIND, g_mods.dev_key);
    } else if (which == CAPTURE_CYCLE) {
        snprintf(g_mods.cycle_key, sizeof(g_mods.cycle_key), "%s", code ? code : "");
        html_storage_set(MODS_KEY_CYCLE_BIND, g_mods.cycle_key);
    } else if (which == CAPTURE_BHOP) {
        ui_bhop_set_bind(code ? code : "");
    }
    mods_refresh_ui();
}

// ---------------------------------------------------------------- rows (make*Row)

// (the texts in the current language: the rows are built when inserted)
static char *setting_rows_html(void) {
    StrBuf sb = {0};
    sb_append(&sb, "<div class=\"settName\" data-force-offline-setting=\"1\" title=\"");
    sb_append_esc(&sb, I18N("kn.mods.forceOfflineInfo", "Starts the game directly in offline test mode."));
    sb_append(&sb, "\">");
    sb_append_esc(&sb, I18N("kn.mods.forceOffline", "Force Offline Mode"));
    sb_append(&sb, " <label class=\"switch\" style=\"margin-left: 10px;\"><input type=\"checkbox\">"
                   "<span class=\"slider\"></span></label></div>"
                   "<div class=\"settName\" data-dev-console-setting=\"1\" title=\"");
    sb_append_esc(&sb, I18N("kn.mods.devConsoleInfo", "Allows opening the developer console in-game"));
    sb_append(&sb, "\">");
    sb_append_esc(&sb, I18N("kn.mods.devConsoleEnable", "Enable Developer Console (`)"));
    sb_append(&sb, " <label class=\"switch\" style=\"margin-left: 10px;\"><input type=\"checkbox\">"
                   "<span class=\"slider\"></span></label></div>");
    return sb_finish(&sb);
}

// mods.ts makes a plain .setHedBind heading + rows (v3.1.1 Change Controls markup); v7.1.4's
// window has collapsible groups instead and no .setHedBind style, so the rows form a v7 group
// of their own ("Developer Tools", collapsible like the others) with v7's key cell markup.
static void bind_row(StrBuf *sb, const char *data, const char *label) {
    sb_appendf(sb, "<div class=\"settName\" %s=\"1\" data-kn-pointer=\"1\">", data);
    sb_append_esc(sb, label);
    sb_append(sb, "<div style=\"float:right\"><span class=\"unbind\" title=\"");
    sb_append_esc(sb, I18N("kn.mods.unbind", "Unbind"));
    sb_append(sb, "\"><i class=\"material-icons\" style=\"font-size:40px;color:var(--red);\">delete_forever</i></span>"
                  "<span class=\"settText floatRNoC keyIcon\" onmouseover=\"playTick()\"></span></div></div>");
}

static char *bind_rows_html(void) {
    StrBuf sb = {0};
    sb_append(&sb, "<div class=\"setHed\" id=\"setBindHed_developer_tools\" data-dev-console-bind-head=\"1\" "
                   "onclick=\"window.windows[6].collapseFolder(this)\"><span class=\"material-icons plusOrMinus\">keyboard_arrow_down</span> ");
    sb_append_esc(&sb, I18N("kn.mods.devTools", "Developer Tools"));
    sb_append(&sb, "</div><div class=\"setBodH\" id=\"setBindBod_developer_tools\">");
    bind_row(&sb, "data-dev-console-bind", I18N("kn.mods.devConsole", "Developer Console"));
    bind_row(&sb, "data-bhop-mode-bind", I18N("kn.mods.bhopMode", "Bhop Mode"));
    bind_row(&sb, "data-cycle-command-bind", I18N("kn.mods.cycleMap", "Cycle Map (/cycle)"));
    sb_append(&sb, "</div>");
    return sb_finish(&sb);
}

static const char *const BIND_ROW_SELECTORS[3] = {
    "[data-dev-console-bind] .settText.floatRNoC.keyIcon",
    "[data-bhop-mode-bind] .settText.floatRNoC.keyIcon",
    "[data-cycle-command-bind] .settText.floatRNoC.keyIcon",
};
static const char *const UNBIND_SELECTORS[3] = {
    "[data-dev-console-bind] .unbind", "[data-bhop-mode-bind] .unbind", "[data-cycle-command-bind] .unbind",
};
static const int ROW_CAPTURE[3] = {CAPTURE_DEV, CAPTURE_BHOP, CAPTURE_CYCLE};

static void set_checkbox(const char *selector, const int on) {
    const HtmlElem input = html_dom_query(0, selector);
    if (input && html_dom_get_checked(input) != on) html_dom_set_checked(input, on);
}

// refreshDevConsoleUi
static void mods_refresh_ui(void) {
    set_checkbox("[data-force-offline-setting] input[type='checkbox']", g_mods.force_offline);
    set_checkbox("[data-dev-console-setting] input[type='checkbox']", g_mods.dev_enabled);
    const char *codes[3] = {g_mods.dev_key, ui_bhop_bind(), g_mods.cycle_key};
    for (int i = 0; i < 3; i++) {
        const HtmlElem span = html_dom_query(0, BIND_ROW_SELECTORS[i]);
        if (!span) continue;
        const char *text = g_mods.capturing == ROW_CAPTURE[i] ? I18N("settings.controls.press", "Press any Key") : ui_mods_format_key_bind(codes[i]);
        if (strcmp(g_mods.shown[i], text) || html_dom_get_text(span, NULL, 0) <= 0) {
            html_dom_set_text(span, text);
            snprintf(g_mods.shown[i], sizeof(g_mods.shown[i]), "%s", text);
        }
    }
}

// ensureDevConsoleSettingRow: Force Offline Mode, then the dev console switch, after the
// "Mouse Flick Fix" (slid_flickClamp) row (v7.1.4: General > Experimental, advanced view).
static void ensure_setting_rows(void) {
    const HtmlElem anchor_input = html_dom_by_id("slid_flickClamp");
    if (!anchor_input) return;
    const HtmlElem anchor_row = html_dom_closest(anchor_input, ".settName");
    if (!anchor_row || html_dom_query(0, "[data-force-offline-setting='1']")) return;
    char *rows = setting_rows_html();
    html_dom_insert_html(anchor_row, HTML_INSERT_AFTER_END, rows);
    free(rows);
    mods_refresh_ui();
}

// ensureDevConsoleBindRow: the "Developer Tools" rows after the Smite row. mods.ts anchors
// at v3.1.1's #cont37 / #cont34, which v7.1.4's Change Controls no longer has (its ids are
// cont<bind>_<index>), so the rows never appear in the reference; KrunkNative anchors them
// at v7's Smite / Noclip key cells (Chat & Toggles > Premium & Host) instead.
static void ensure_bind_rows(void) {
    HtmlElem anchor = html_dom_by_id("contpremium_3");
    if (!anchor) anchor = html_dom_by_id("contpremium_0");
    if (!anchor) anchor = html_dom_by_id("cont37");
    if (!anchor) anchor = html_dom_by_id("cont34");
    if (!anchor) return;
    HtmlElem anchor_row = html_dom_closest(anchor, ".setBodH"); // after the Premium & Host group
    if (!anchor_row) anchor_row = html_dom_closest(anchor, ".settName");
    if (!anchor_row || html_dom_query(0, "[data-dev-console-bind-head='1']")) return;
    char *rows = bind_rows_html();
    html_dom_insert_html(anchor_row, HTML_INSERT_AFTER_END, rows);
    free(rows);
    for (int i = 0; i < 3; i++) g_mods.shown[i][0] = 0;
    mods_refresh_ui();
}

// The switches' click listeners: the checkbox state the page now shows. A switch input we
// have not seen yet (a fresh render of the settings tab: the captured window templates carry
// both rows, unchecked, so ensure_setting_rows does not insert them) is first set from the
// stored value (and marked data-kn-synced); only a later difference is the player's click. Reading the fresh unchecked
// box as a click stored devConsoleEnabled = false every time Settings opened.
static void poll_switch(const char *selector, const int value, void (*set)(int)) {
    const HtmlElem input = html_dom_query(0, selector);
    if (!input) return;
    char mark[4];
    if (html_dom_get_attr(input, "data-kn-synced", mark, sizeof(mark)) < 0) {
        html_dom_set_attr(input, "data-kn-synced", "1");
        if (html_dom_get_checked(input) != value) html_dom_set_checked(input, value);
        return;
    }
    if (html_dom_get_checked(input) != value) set(html_dom_get_checked(input));
}

static void poll_switches(void) {
    poll_switch("[data-dev-console-setting] input[type='checkbox']", g_mods.dev_enabled, mods_set_dev_enabled);
    poll_switch("[data-force-offline-setting] input[type='checkbox']", g_mods.force_offline, mods_set_force_offline);
}

void ui_mods_tick(Client *client) {
    (void) client;
    mods_load();
    if (!html_ui_is_ready()) return;
    ensure_setting_rows();
    ensure_bind_rows();
    poll_switches();
}

// ---------------------------------------------------------------- pointer / capture

static int rect_hit(const HtmlElem el, const float x, const float y) {
    HtmlRect r;
    return el && html_dom_get_rect(el, &r) && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

int ui_mods_capturing(void) {
    return g_mods.capturing != CAPTURE_NONE;
}

// begin*BindCapture
static void begin_capture(const int which) {
    if (g_mods.capturing) return;
    g_mods.capturing = which;
    mods_refresh_ui();
}

int ui_mods_pointer(const int down, const float x, const float y) {
    for (int i = 0; i < 3; i++) {
        const HtmlElem span = html_dom_query(0, BIND_ROW_SELECTORS[i]);
        const HtmlElem unbind = html_dom_query(0, UNBIND_SELECTORS[i]);
        const HtmlElem hit = rect_hit(span, x, y) ? span : rect_hit(unbind, x, y) ? unbind : 0;
        if (!hit) continue;
        if (down) {
            g_mods.pressed = hit;
            return 1;
        }
        if (g_mods.pressed != hit) break;
        g_mods.pressed = 0;
        if (hit == unbind) mods_set_bind(ROW_CAPTURE[i], "");
        else begin_capture(ROW_CAPTURE[i]);
        return 1;
    }
    if (!down) g_mods.pressed = 0;
    return 0;
}

// The capture listeners (document keydown, capture phase): the next key's code, Escape cancels.
static int capture_key(const int key) {
    const int which = g_mods.capturing;
    g_mods.capturing = CAPTURE_NONE;
    if (key != GLFW_KEY_ESCAPE) mods_set_bind(which, ui_mods_key_code(key));
    else mods_refresh_ui();
    return 1;
}

// ---------------------------------------------------------------- keydown (mods.ts)

static void toggle_developer_console(void) {
    if (!g_mods.dev_enabled) return;
    if (app_mode() == APP_MODE_GAME) {           // the editor / hub / viewer pages have no menu or game
        if (ui_console_is_open()) menu_play();   // instructionHolder?.click?.()
        else menu_request_open(1);               // document.exitPointerLock?.()
    }
    ui_console_toggle(1);
}

// The v7 Parkour reset binds (resetKey B / resetLastKey N by default) win over the mod's bhop
// toggle (KeyB by default) in modes that reset (mode.allowReset): v7 has no bhop mod, so there the
// key keeps its v7 meaning and the toggle does not fire with it.
static int parkour_reset_key(const Client *client, const int key) {
    const GameMode *mode = client ? client->game.mode : NULL;
    if (!mode || !mode->config.allow_reset) return 0;
    const int code = ui_controls_dom_key(key);
    if (code < 0) return 0;
    static const int ids[2] = {UI_CONTROL_RESET, UI_CONTROL_RESET_LAST};
    for (int i = 0; i < 2; i++) {
        if (code == ui_controls_bind(ids[i]) || code == ui_controls_bind_alt(ids[i])) return 1;
    }
    return 0;
}

int ui_mods_toggle_key_event(const int key, const int action, const int typing) {
    mods_load();
    if (action != GLFW_PRESS || g_mods.capturing) return 0;
    if (!g_mods.dev_enabled || !ui_mods_code_matches(g_mods.dev_key, key)) return 0;
    if (typing && !ui_console_is_open()) return 0; // another field keeps the character
    toggle_developer_console();
    return 1;
}

int ui_mods_key_event(Client *client, const int key, const int action, const int mods, const int typing) {
    (void) mods;
    mods_load();
    if (action != GLFW_PRESS) return 0;  // if (e.repeat) return;
    if (g_mods.capturing) return capture_key(key);
    const int visible = ui_console_is_open();

    if (ui_mods_toggle_key_event(key, action, typing)) return 1;
    if (!typing && !visible && ui_mods_code_matches(g_mods.cycle_key, key)) {
        if (client) client_send_cycle_packet(client); // window.io.send("cycle")
        return 1;
    }
    if (!typing && !visible && ui_mods_code_matches(ui_bhop_bind(), key) && !parkour_reset_key(client, key)) {
        ui_bhop_toggle();
        return 1;
    }
    // mods.ts switch: F1 clearLog, F4 "/", F5 reload, F6 editor, T Ctrl TP. KrunkNative runs none of
    // them while a text field has focus (v3.1.1 still let F5 / F6 through there).
    if (typing || visible) return 0;
    return ui_mod_keys_keydown(client, key, 0);
}
