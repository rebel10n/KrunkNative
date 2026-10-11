// Krunker's keybinds (CONTROLS, 132.js in the unpacked v3.1.1 client) and the
// in-game key/mouse routing built on them:
//   - the binding table: ids 0..37 of windows[6] ("Change Controls", Krunker window 7),
//     DOM keyCodes (mouse buttons 10000 + event.which, the wheel 20000), defaults
//     and the localStorage keys Krunker persists them under (cont_0, cont_jumpKey, ...,
//     cont_27_set for the Toggle rows) through html_storage_*;
//   - the window markup (gen), changeCont / unbindCont / changeContSet and the
//     "Press any Key" capture (CONTROLS N(): the next key, side/middle mouse button or
//     wheel turn becomes the bind);
//   - the bound-key state the game samples every frame (CONTROLS.keys / didPressed /
//     UCUz1 / DNCv5 / aba6 / wDG6) and the pressButton()/releaseButton() hotkeys;
//   - chat focus (#chatInput, the chat key opens/sends) and the input settings the
//     engine applies (sensitivity, invert, fpsFOV, scrollDir, weapon visibility).
// All of it is shared by the native and web clients: main.c feeds GLFW events (the
// emscripten GLFW port reports DOM keys as GLFW keys) and the page's focusChat events.
#include <ui_logic.h>
#include <html_ui.h>
#include <spectate.h> // spectator keys (pressButton's window.Bf5 block, client/src/spectate.c)
#include <gamepad.h>  // Gamepad.active (controller: client/src/gamepad.c)
#include <ui_mobile.h> // touch look (#mobileUI, client/src/ui/ui_mobile.c)
#include <console.h>   // the developer console takes the keyboard while open (ui_console.c)
#include <ui_terminal.h> // map terminals: "int" on the interact keys, #terminalInput typing (ui_terminal.c)
#include <voice_chat.h> // voiceKey -> toggleRecord (client/src/voice.c)
#include <GLFW/glfw3.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KEY_MOUSE_BASE 10000   // Krunker: 10000 + MouseEvent.which (2 middle, 4 back, 5 forward)
#define KEY_SCROLL 20000       // Krunker: the mouse wheel as a key
#define KEY_UNBOUND (-1)
#define KEY_SLOTS 262          // 0..255 keyCodes, 256..260 M1..M5, 261 SCROLL
#define TOGGLE_FIRST UI_CONTROL_TOGGLE0
#define TOGGLE_COUNT 6
#define SETTING_KEY_MAX 48
#define CHAT_MAX 256

// aU.keyboardMap (2.js): Krunker's DOM keyCode -> name table, 256 entries.
static const char *const KEYBOARD_MAP[256] = {
    "", "", "", "CANCEL", "", "", "HELP", "", "BACK_SPACE", "TAB", "", "", "CLEAR", "ENTER", "ENTER_SPECIAL", "",
    "SHIFT", "CONTROL", "ALT", "PAUSE", "CAPS_LOCK", "KANA", "EISU", "JUNJA", "FINAL", "HANJA", "", "ESCAPE",
    "CONVERT", "NONCONVERT", "ACCEPT", "MODECHANGE", "SPACE", "PAGE_UP", "PAGE_DOWN", "END", "HOME", "LEFT", "UP",
    "RIGHT", "DOWN", "SELECT", "PRINT", "EXECUTE", "PRINTSCREEN", "INSERT", "DELETE", "", "0", "1", "2", "3", "4",
    "5", "6", "7", "8", "9", "COLON", "SEMICOLON", "LESS_THAN", "EQUALS", "GREATER_THAN", "QUESTION_MARK", "AT",
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V",
    "W", "X", "Y", "Z", "OS_KEY", "", "CONTEXT_MENU", "", "SLEEP", "NUMPAD0", "NUMPAD1", "NUMPAD2", "NUMPAD3",
    "NUMPAD4", "NUMPAD5", "NUMPAD6", "NUMPAD7", "NUMPAD8", "NUMPAD9", "MULTIPLY", "ADD", "SEPARATOR", "SUBTRACT",
    "DECIMAL", "DIVIDE", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12", "F13", "F14",
    "F15", "F16", "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24", "", "", "", "", "", "", "", "",
    "NUM_LOCK", "SCROLL_LOCK", "WIN_OEM_FJ_JISHO", "WIN_OEM_FJ_MASSHOU", "WIN_OEM_FJ_TOUROKU", "WIN_OEM_FJ_LOYA",
    "WIN_OEM_FJ_ROYA", "", "", "", "", "", "", "", "", "", "CIRCUMFLEX", "EXCLAMATION", "DOUBLE_QUOTE", "HASH",
    "DOLLAR", "PERCENT", "AMPERSAND", "UNDERSCORE", "OPEN_PAREN", "CLOSE_PAREN", "ASTERISK", "PLUS", "PIPE",
    "HYPHEN_MINUS", "OPEN_CURLY_BRACKET", "CLOSE_CURLY_BRACKET", "TILDE", "", "", "", "", "VOLUME_MUTE",
    "VOLUME_DOWN", "VOLUME_UP", "", "", ";", "=", ",", "MINUS", "PERIOD", "SLASH", "BACK_QUOTE", "", "", "", "", "",
    "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "[", "\\", "]", "QUOTE", "",
    "META", "ALTGR", "", "WIN_ICO_HELP", "WIN_ICO_00", "", "WIN_ICO_CLEAR", "", "", "WIN_OEM_RESET", "WIN_OEM_JUMP",
    "WIN_OEM_PA1", "WIN_OEM_PA2", "WIN_OEM_PA3", "WIN_OEM_WSCTRL", "WIN_OEM_CUSEL", "WIN_OEM_ATTN",
    "WIN_OEM_FINISH", "WIN_OEM_COPY", "WIN_OEM_AUTO", "WIN_OEM_ENLW", "WIN_OEM_BACKTAB", "ATTN", "CRSEL", "EXSEL",
    "EREOF", "PLAY", "ZOOM", "", "PA1", "WIN_OEM_CLEAR", "",
};
// The `toggle: true` settings (bf.list order) offered by the Toggles rows: value, label.
static const char *const TOGGLE_SETTINGS[][2] = {
    {"antiAlias", "Antialiasing [Performance]"},
    {"lowSpec", "Low Spec [Performance]"},
    {"noTex", "No Textures [Performance]"},
    {"mapDet", "Map Details [Performance]"},
    {"particles", "Particles [Performance]"},
    {"shadows", "Shadows [Performance]"},
    {"shadowsDynamic", "Dynamic Shadows [Performance]"},
    {"ambientShading", "Ambient Shading [Performance]"},
    {"showTrails", "Bullet Trails [Performance]"},
    {"yourTrails", "Your Trails [Performance]"},
    {"muzzleFlash", "Muzzle Flash [Performance]"},
    {"sniperFlap", "Sniper Flap [Performance]"},
    {"textureAnim", "Texture Animations [Performance]"},
    {"objectAnim", "Object Animations [Performance]"},
    {"screenShake", "Screen Shake [Performance]"},
    {"weaponShine", "Weapons Shine [Performance]"},
    {"showUI", "Show UI [Interface]"},
    {"oldScoreboard", "Use Old Scoreboard [Interface]"},
    {"oldBrowser", "Use Old Browser [Interface]"},
    {"disableRarityAnim", "Disable Rarity Animations [Interface]"},
    {"showPfp", "Show Profile Pictures [Interface]"},
    {"dynamicHP", "Dynamic HP Bars [Interface]"},
    {"showHitInd", "Show Hit Indicators [Interface]"},
    {"showDMG", "Show Damage [Interface]"},
    {"showKills", "Show Kill Feed [Interface]"},
    {"showKillC", "Show Kill Counter [Interface]"},
    {"showDeaths", "Show Death Counter [Interface]"},
    {"showScore", "Show Score Counter [Interface]"},
    {"showStreak", "Show Streak Counter [Interface]"},
    {"showPing", "Show Ping [Interface]"},
    {"showFPS", "Show FPS [Interface]"},
    {"showSpeed", "Show Movement Speed [Interface]"},
    {"showMedals", "Show Medals [Interface]"},
    {"healthNum", "Nametag Health Number [Interface]"},
    {"chatFilter", "Profanity Filter [Chat]"},
    {"showMessages", "Show Player Messages [Chat]"},
    {"showUnboxings", "Show Unboxings [Chat]"},
    {"chatTextOutline", "Chat Text Outline [Chat]"},
    {"crosshairUseOpacity", "Use Custom Opacity [Crosshair]"},
    {"crosshairAlways", "Always Show [Crosshair]"},
    {"crosshairDot", "Dot [Crosshair]"},
    {"hitm", "Show [Hitmarker]"},
    {"slidCont", "Non FPS Sliding [Gameplay]"},
    {"scrollDir", "Scroll Direction [Gameplay]"},
    {"challMode", "Challenge Mode [Gameplay]"},
    {"invertY", "Invert Y-Axis [Gameplay]"},
    {"streamMode", "Streamer Mode [Gameplay Privacy]"},
    {"anonMode", "Anonymous Mode [Gameplay Privacy]"},
    {"leftHand", "Left Handed [View Model]"},
    {"aimAnim", "Weapon Aim Animation [View Model]"},
    {"hideADS", "Hide Weapon on ADS [View Model]"},
    {"showHands", "Show Hands [View Model]"},
    {"showWeapon", "Show Primary [View Model]"},
    {"showWeaponSec", "Show Secondary [View Model]"},
    {"showWeaponMel", "Show Melee [View Model]"},
    {"roundArms", "Rounded Arms [View Model]"},
    {"showPScore", "Show Popup Score [Editing]"},
    {"scoreShadow", "Popup Score Shadow [Editing]"},
    {"bulletTracers", "Bullet Tracers [Editing]"},
    {"scopeBorders", "Scope Borders [Customization]"},
    {"useDamageOverlay", "Use Damage Overlay [Customization]"},
};

// windows[6].gen() rows, in Krunker's order: label (i18n windows.controls.*), bind id,
// section header written before the row (NULL none). Toggle rows (27..32) get the
// setting <select> instead of a label.
typedef struct {
    const char *label;
    int id;
    const char *header;
} ControlRow;

static const ControlRow ROWS[] = {
    {"Forward", 0, "Movement"}, {"Backward", 1, NULL}, {"Left", 2, NULL}, {"Right", 3, NULL},
    {"Jump", 4, NULL}, {"Crouch", 5, NULL},
    {"Reload", 8, "Weapon"}, {"Aim", 10, NULL}, {"Shoot", 25, NULL}, {"Primary Weapon", 13, NULL},
    {"Melee Weapon", 6, NULL}, {"Secondary Weapon", 7, NULL}, {"Equipment", 33, NULL}, {"Inspect", 14, NULL},
    {"Spray", 9, NULL},
    {"Interact", 16, "Interaction"}, {"Secondary Interact", 23, NULL}, {"Drop", 17, NULL},
    {"Chat", 11, "Interface & Audio"}, {"Voice", 12, NULL}, {"Player List", 15, NULL}, {"Leaderboard", 26, NULL},
    {"Streak 1", 18, "Killstreaks & Prop Hunt"}, {"Streak 2", 19, NULL}, {"Streak 3", 20, NULL},
    {"Streak 4", 21, NULL}, {"Streak 5", 22, NULL},
    {"Toggle Weapon Visibility", 24, "Toggles"}, {NULL, 27, NULL}, {NULL, 28, NULL}, {NULL, 29, NULL},
    {NULL, 30, NULL}, {NULL, 31, NULL}, {NULL, 32, NULL},
    {"Noclip", 34, "Premium & Host"}, {"Godmode", 35, NULL}, {"Kill", 36, NULL}, {"Smite", 37, NULL},
};

// CONTROLS.keyInfo: the binds.* name behind an id (its storage key is cont_<name>);
// NULL = an array bind stored as cont_<id> (moveKeys 0-3, streakKeys 18-22,
// toggleKeys 27-32, premiumKeys 34-37).
static const char *const KEY_INFO[UI_CONTROL_COUNT] = {
    NULL, NULL, NULL, NULL, "jumpKey", "crouchKey", "meleeKey", "swapKey", "reloadKey", "sprayKey",
    "aimKey", "chatKey", "voiceKey", "primKey", "inspKey", "pListKey", "interactKey", "dropKey",
    NULL, NULL, NULL, NULL, NULL, "interactSecKey", "wepVisKey", "shootKey", "sBoardKey",
    NULL, NULL, NULL, NULL, NULL, NULL, "equipKey", NULL, NULL, NULL, NULL,
};

// CONTROLS.binds.*.def by id (DOM keyCodes).
static const int DEFAULT_BINDS[UI_CONTROL_COUNT] = {
    87, 83, 65, 68,              // W S A D
    32, 16, 81, 69, 82, 70,      // jump SPACE, crouch SHIFT, melee Q, swap E, reload R, spray F
    221, 13, 86, 84, 88, 18,     // aim ], chat ENTER, voice V, prim T, inspect X, player list ALT
    71, 90,                      // interact G, drop Z
    49, 50, 51, 52, 53,          // streaks 1-5
    72, -1, 220, 9,              // interactSec H, wepVis unbound, shoot \, leaderboard TAB
    -1, -1, -1, -1, -1, -1,      // toggles
    67,                          // equip C
    -1, -1, -1, -1,              // premium
};


static struct {
    int loaded;
    int binds[UI_CONTROL_COUNT];
    char toggle_sets[TOGGLE_COUNT][SETTING_KEY_MAX]; // "" = -1 (None)
    int scroll_bound;        // !bMoQ7: some bind uses the wheel, so it does not swap weapons
    int input_changer;       // id waiting for "Press any Key", -1 none

    unsigned char keys[KEY_SLOTS];
    unsigned char did_pressed[KEY_SLOTS];
    int shoot;               // UCUz1: left mouse button
    int aim;                 // DNCv5: right mouse button
    int pending_swap;        // aba6: 1 swap(secondary) 2 melee 3 primary 4 equipment
    int scroll_pressed;      // the wheel "key" is down until the end of this input frame

    int chat_focused;        // #chatInput has focus (focusChat(1) / focusChat())
    int swallowed_key;       // web: a capture consumed this key event before the page sees it
    unsigned char logged[UI_CONTROL_COUNT];

    UiInputSettings settings; // Krunker defaults until client_apply_setting pushes values

    // Controller (CONTROLS.cntrlInput, inputType, a4/a5, speedLmt; client/src/gamepad.c)
    UiControllerInput cntrl;
    int input_cntrl;         // inputType == "cntrlInput" (the last look input was the right stick)
    float look_x, look_y;    // a4/a5 added by fakeMouse(2) since the last updateMouseInp
    float speed_lmt;         // speedLmt (gradSpeed); 1 = full speed
} g_ctl = {
    .input_changer = -1,
    .settings = {90.0f, 1, 1, 1, 1, 0, 0.8f, 1.0f, 1.0f, 1},
    // cntrlInput after the settings' set(): sensitivity*Cntrl x5, deadZoneL 0.3 (the
    // setting's val overrides cntrlInput's own 0.25), deadZoneR 0.25, triggerThres 0.1,
    // invertYCntrl off, vibration on, gradSpeed off.
    .cntrl = {5.0f, 5.0f, 5.0f, 5.0f, 0.3f, 0.25f, 0.1f, 0, 1, 0},
    .speed_lmt = 1.0f,
};

// ---------------------------------------------------------------- key codes

// GLFW key -> DOM keyCode (what Krunker stores and names). Printable keys share codes;
// punctuation uses Chrome's keyCodes (the ones aU.keyboardMap names). The emscripten
// GLFW port maps the browser's keyCode to these GLFW keys, so the inverse holds there too.
int ui_controls_dom_key(const int key) {
    if ((key >= GLFW_KEY_0 && key <= GLFW_KEY_9) || (key >= GLFW_KEY_A && key <= GLFW_KEY_Z) || key == GLFW_KEY_SPACE) return key;
    if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F24) return 112 + (key - GLFW_KEY_F1);
    if (key >= GLFW_KEY_KP_0 && key <= GLFW_KEY_KP_9) return 96 + (key - GLFW_KEY_KP_0);
    switch (key) {
        case GLFW_KEY_APOSTROPHE: return 222;
        case GLFW_KEY_COMMA: return 188;
        case GLFW_KEY_MINUS: return 189;
        case GLFW_KEY_PERIOD: return 190;
        case GLFW_KEY_SLASH: return 191;
        case GLFW_KEY_SEMICOLON: return 186;
        case GLFW_KEY_EQUAL: return 187;
        case GLFW_KEY_LEFT_BRACKET: return 219;
        case GLFW_KEY_BACKSLASH: return 220;
        case GLFW_KEY_RIGHT_BRACKET: return 221;
        case GLFW_KEY_GRAVE_ACCENT: return 192;
        case GLFW_KEY_WORLD_1: return 226;
        case GLFW_KEY_ESCAPE: return 27;
        case GLFW_KEY_ENTER: case GLFW_KEY_KP_ENTER: return 13;
        case GLFW_KEY_TAB: return 9;
        case GLFW_KEY_BACKSPACE: return 8;
        case GLFW_KEY_INSERT: return 45;
        case GLFW_KEY_DELETE: return 46;
        case GLFW_KEY_RIGHT: return 39;
        case GLFW_KEY_LEFT: return 37;
        case GLFW_KEY_DOWN: return 40;
        case GLFW_KEY_UP: return 38;
        case GLFW_KEY_PAGE_UP: return 33;
        case GLFW_KEY_PAGE_DOWN: return 34;
        case GLFW_KEY_HOME: return 36;
        case GLFW_KEY_END: return 35;
        case GLFW_KEY_CAPS_LOCK: return 20;
        case GLFW_KEY_SCROLL_LOCK: return 145;
        case GLFW_KEY_NUM_LOCK: return 144;
        case GLFW_KEY_PRINT_SCREEN: return 44;
        case GLFW_KEY_PAUSE: return 19;
        case GLFW_KEY_KP_DECIMAL: return 110;
        case GLFW_KEY_KP_DIVIDE: return 111;
        case GLFW_KEY_KP_MULTIPLY: return 106;
        case GLFW_KEY_KP_SUBTRACT: return 109;
        case GLFW_KEY_KP_ADD: return 107;
        case GLFW_KEY_KP_EQUAL: return 187;
        case GLFW_KEY_LEFT_SHIFT: case GLFW_KEY_RIGHT_SHIFT: return 16;
        case GLFW_KEY_LEFT_CONTROL: case GLFW_KEY_RIGHT_CONTROL: return 17;
        case GLFW_KEY_LEFT_ALT: case GLFW_KEY_RIGHT_ALT: return 18;
        case GLFW_KEY_LEFT_SUPER: return 91;
        case GLFW_KEY_RIGHT_SUPER: return 92;
        case GLFW_KEY_MENU: return 93;
        default: return -1;
    }
}

// GLFW mouse button -> Krunker's 10000 + MouseEvent.which (left 1, middle 2, right 3,
// back 4, forward 5).
static int mouse_code(const int button) {
    switch (button) {
        case GLFW_MOUSE_BUTTON_LEFT: return KEY_MOUSE_BASE + 1;
        case GLFW_MOUSE_BUTTON_MIDDLE: return KEY_MOUSE_BASE + 2;
        case GLFW_MOUSE_BUTTON_RIGHT: return KEY_MOUSE_BASE + 3;
        default: return button >= 3 && button <= 7 ? KEY_MOUSE_BASE + button + 1 : -1;
    }
}

static int key_slot(const int code) {
    if (code >= 0 && code < 256) return code;
    if (code > KEY_MOUSE_BASE && code <= KEY_MOUSE_BASE + 5) return 255 + (code - KEY_MOUSE_BASE);
    if (code == KEY_SCROLL) return 261;
    return -1;
}

// aU.getKeyName(l)
const char *ui_controls_key_name(const int code, char *buf, const size_t n) {
    if (!buf || !n) return "";
    if (code < 0) snprintf(buf, n, "UNBOUND");
    else if (code == KEY_SCROLL) snprintf(buf, n, "SCROLL");
    else if (code > KEY_MOUSE_BASE) snprintf(buf, n, "M%d", code - KEY_MOUSE_BASE);
    else if (code < (int) (sizeof(KEYBOARD_MAP) / sizeof(KEYBOARD_MAP[0]))) snprintf(buf, n, "%s", KEYBOARD_MAP[code]);
    else snprintf(buf, n, "undefined"); // keyboardMap[l] past the table
    return buf;
}

// ---------------------------------------------------------------- binding table

static void storage_key(const int id, char *buf, const size_t n) {
    if (KEY_INFO[id]) snprintf(buf, n, "cont_%s", KEY_INFO[id]);
    else snprintf(buf, n, "cont_%d", id);
}

// CONTROLS.checkForScroll(): bMoQ7 is false while any bind uses the wheel.
static void check_for_scroll(void) {
    g_ctl.scroll_bound = 0;
    for (int i = 0; i < UI_CONTROL_COUNT; i++) {
        if (g_ctl.binds[i] == KEY_SCROLL) g_ctl.scroll_bound = 1;
    }
}

// parseInt(getSavedVal(key) || def)
static int saved_int(const char *key, const int def) {
    char buf[64];
    if (html_storage_get(key, buf, sizeof(buf)) < 0 || !buf[0]) return def;
    char *end;
    const long v = strtol(buf, &end, 10);
    return end == buf ? def : (int) v;
}

// CONTROLS.binds (getSavedVal) + setupBinds() (toggleSets)
void ui_controls_load(void) {
    char key[64];
    for (int i = 0; i < UI_CONTROL_COUNT; i++) {
        storage_key(i, key, sizeof(key));
        g_ctl.binds[i] = saved_int(key, DEFAULT_BINDS[i]);
    }
    for (int i = 0; i < TOGGLE_COUNT; i++) {
        snprintf(key, sizeof(key), "cont_%d_set", TOGGLE_FIRST + i);
        char value[SETTING_KEY_MAX] = "";
        if (html_storage_get(key, value, sizeof(value)) < 0 || !strcmp(value, "-1")) value[0] = 0;
        snprintf(g_ctl.toggle_sets[i], sizeof(g_ctl.toggle_sets[i]), "%s", value);
    }
    check_for_scroll();
    g_ctl.loaded = 1;
}

static void ensure_loaded(void) {
    if (!g_ctl.loaded) ui_controls_load();
}

int ui_controls_bind(const int id) {
    ensure_loaded();
    return id >= 0 && id < UI_CONTROL_COUNT ? g_ctl.binds[id] : KEY_UNBOUND;
}

static int bind_is(const int id, const int code) {
    return code >= 0 && g_ctl.binds[id] == code;
}

static void set_bind(const int id, const int code) {
    char key[64], value[16];
    g_ctl.binds[id] = code;
    check_for_scroll();
    storage_key(id, key, sizeof(key));
    snprintf(value, sizeof(value), "%d", code);
    html_storage_set(key, value); // saveVal("cont_" + inputChanger, keyCode)
}

// resetSettings(): every cont_* key is removed, then setupBinds() + resetBinds().
void ui_controls_reset(void) {
    char key[64];
    for (int i = 0; i < UI_CONTROL_COUNT; i++) {
        storage_key(i, key, sizeof(key));
        html_storage_set(key, NULL);
        g_ctl.binds[i] = DEFAULT_BINDS[i];
    }
    for (int i = 0; i < TOGGLE_COUNT; i++) {
        snprintf(key, sizeof(key), "cont_%d_set", TOGGLE_FIRST + i);
        html_storage_set(key, NULL);
        g_ctl.toggle_sets[i][0] = 0;
    }
    g_ctl.input_changer = -1;
    check_for_scroll();
    g_ctl.loaded = 1;
    menu_rerender_if_open(7);
}

// ---------------------------------------------------------------- export / import

// CONTROLS.export() key order; array binds and the id of their element 0.
typedef struct {
    const char *name;
    int first;   // id of element 0 (arrays) or the id itself
    int count;   // 0 = scalar bind
} BindGroup;

static const BindGroup BIND_GROUPS[] = {
    {"jumpKey", 4, 0}, {"crouchKey", 5, 0}, {"meleeKey", 6, 0}, {"swapKey", 7, 0}, {"primKey", 13, 0},
    {"reloadKey", 8, 0}, {"sprayKey", 9, 0}, {"inspKey", 14, 0}, {"aimKey", 10, 0}, {"shootKey", 25, 0},
    {"chatKey", 11, 0}, {"voiceKey", 12, 0}, {"pListKey", 15, 0}, {"interactKey", 16, 0},
    {"interactSecKey", 23, 0}, {"dropKey", 17, 0}, {"wepVisKey", 24, 0}, {"sBoardKey", 26, 0},
    {"streakKeys", 18, 5}, {"moveKeys", 0, 4}, {"toggleKeys", 27, 6}, {"equipKey", 33, 0},
    {"premiumKeys", 34, 4},
};

// CONTROLS.export(): binds that differ from their default; array binds as arrays whose
// default entries are holes (JSON null), dropped when nothing differs. NULL when every
// bind is the default (exportSettings then writes no "controls" key).
cJSON *ui_controls_export_json(void) {
    ensure_loaded();
    cJSON *out = cJSON_CreateObject();
    for (size_t g = 0; g < sizeof(BIND_GROUPS) / sizeof(BIND_GROUPS[0]); g++) {
        const BindGroup *b = &BIND_GROUPS[g];
        if (!b->count) {
            if (g_ctl.binds[b->first] != DEFAULT_BINDS[b->first]) cJSON_AddNumberToObject(out, b->name, g_ctl.binds[b->first]);
            continue;
        }
        int last = -1;
        for (int i = 0; i < b->count; i++) {
            if (g_ctl.binds[b->first + i] != DEFAULT_BINDS[b->first + i]) last = i;
        }
        if (last < 0) continue;
        cJSON *arr = cJSON_AddArrayToObject(out, b->name);
        for (int i = 0; i <= last; i++) {
            const int v = g_ctl.binds[b->first + i];
            cJSON_AddItemToArray(arr, v != DEFAULT_BINDS[b->first + i] ? cJSON_CreateNumber(v) : cJSON_CreateNull());
        }
    }
    if (!out->child) {
        cJSON_Delete(out);
        return NULL;
    }
    return out;
}

// settings.import(): the "controls" object. Array entries that are null take the
// default; ids past the array keep their bind. KrunkNative deviation: v3.1.1 loops over
// `.length` for every entry, so its scalar binds (jumpKey: 70) are silently skipped;
// here they are imported too.
void ui_controls_import_json(const cJSON *controls) {
    ensure_loaded();
    if (!cJSON_IsObject(controls)) return;
    for (size_t g = 0; g < sizeof(BIND_GROUPS) / sizeof(BIND_GROUPS[0]); g++) {
        const BindGroup *b = &BIND_GROUPS[g];
        const cJSON *v = cJSON_GetObjectItem(controls, b->name);
        if (!v) continue;
        if (!b->count) {
            if (cJSON_IsNumber(v)) set_bind(b->first, (int) cJSON_GetNumberValue(v));
            continue;
        }
        if (!cJSON_IsArray(v)) continue;
        int i = 0;
        const cJSON *item;
        cJSON_ArrayForEach(item, v) {
            if (i >= b->count) break;
            const int id = b->first + i++;
            set_bind(id, cJSON_IsNumber(item) ? (int) cJSON_GetNumberValue(item) : DEFAULT_BINDS[id]);
        }
    }
    g_ctl.input_changer = -1;
    menu_rerender_if_open(7);
}

// ---------------------------------------------------------------- the window

static void append_key_name(StrBuf *sb, const int code) {
    char name[32];
    sb_append_esc(sb, ui_controls_key_name(code, name, sizeof(name)));
}

// windows[6].gen(), serialised the way the page holds it (double quotes, selected="").
char *ui_controls_window_html(void) {
    ensure_loaded();
    StrBuf sb = {0};
    for (size_t r = 0; r < sizeof(ROWS) / sizeof(ROWS[0]); r++) {
        const ControlRow *row = &ROWS[r];
        if (row->header) {
            sb_appendf(&sb, "<div class=\"setHedBind\"%s>", r == 0 ? " style=\"margin-top:0px;\"" : "");
            sb_append_esc(&sb, row->header);
            sb_append(&sb, "</div>");
        }
        sb_append(&sb, "<div class=\"settName\">");
        if (row->label) {
            sb_append_esc(&sb, row->label);
        } else {
            const char *set = g_ctl.toggle_sets[row->id - TOGGLE_FIRST];
            sb_appendf(&sb, "Toggle <select class=\"setToggle\" onchange=\"changeContSet(%d, this.value)\">", row->id);
            sb_appendf(&sb, "<option value=\"-1\"%s>None</option>", set[0] ? "" : " selected=\"\"");
            for (size_t t = 0; t < sizeof(TOGGLE_SETTINGS) / sizeof(TOGGLE_SETTINGS[0]); t++) {
                sb_append(&sb, "<option value=\"");
                sb_append_esc(&sb, TOGGLE_SETTINGS[t][0]);
                sb_appendf(&sb, "\"%s>", !strcmp(set, TOGGLE_SETTINGS[t][0]) ? " selected=\"\"" : "");
                sb_append_esc(&sb, TOGGLE_SETTINGS[t][1]);
                sb_append(&sb, "</option>");
            }
            sb_append(&sb, "</select>");
        }
        sb_appendf(&sb, "<span class=\"unbind\" onclick=\"unbindCont(%d)\"><i class=\"material-icons\" "
                        "style=\"font-size:40px;color:var(--red);\">delete_forever</i></span>"
                        "<span class=\"settText floatRNoC keyIcon\" id=\"cont%d\" onmouseover=\"playTick()\" "
                        "onclick=\"changeCont(%d)\">", row->id, row->id, row->id);
        append_key_name(&sb, g_ctl.binds[row->id]);
        sb_append(&sb, "</span></div>");
    }
    return sb_finish(&sb);
}

static void set_cont_text(const int id, const char *text) {
    char elem_id[16];
    snprintf(elem_id, sizeof(elem_id), "cont%d", id);
    HtmlElem el = html_dom_by_id(elem_id);
    if (el) html_dom_set_html(el, text);
}

static int valid_id(const int id) {
    return id >= 0 && id < UI_CONTROL_COUNT;
}

// window.changeCont(id, noRender): re-render the window, then wait for a key.
void ui_controls_change(const int id, const int no_render) {
    ensure_loaded();
    if (!valid_id(id)) return;
    if (!no_render) menu_show_window(7, 1);
    g_ctl.input_changer = id;
    set_cont_text(id, "Press any Key");
}

// window.unbindCont(id, noRender)
void ui_controls_unbind(const int id, const int no_render) {
    ensure_loaded();
    g_ctl.input_changer = -1;
    if (!valid_id(id)) return;
    if (!no_render) menu_show_window(7, 1);
    set_cont_text(id, "UNBOUND");
    set_bind(id, KEY_UNBOUND);
}

// window.changeContSet(id, setting): which setting a Toggle row flips.
void ui_controls_change_set(const int id, const char *setting) {
    ensure_loaded();
    if (id < TOGGLE_FIRST || id >= TOGGLE_FIRST + TOGGLE_COUNT) return;
    const char *value = setting && *setting ? setting : "-1";
    snprintf(g_ctl.toggle_sets[id - TOGGLE_FIRST], SETTING_KEY_MAX, "%s", strcmp(value, "-1") ? value : "");
    char key[32];
    snprintf(key, sizeof(key), "cont_%d_set", id);
    html_storage_set(key, value);
    menu_show_window(7, 1);
}

// KrunkNative deviation: Krunker keeps inputChanger after the controls window closes, so
// the next key pressed anywhere (even in game) is bound and window 7 reopens. Here the
// capture ends with the window.
int ui_controls_capturing(void) {
    if (g_ctl.input_changer < 0) return 0;
    const MenuUi *ui = menu_ui();
    if (ui->active_window != 7 || !menu_window_is_open()) {
        g_ctl.input_changer = -1;
        return 0;
    }
    return 1;
}

// CONTROLS N() with inputChanger set: store the code and re-render the window.
static void capture_bind(const int code) {
    const int id = g_ctl.input_changer;
    g_ctl.input_changer = -1;
    set_bind(id, code);
    char name[32];
    printf("[controls] bind %d (%s) = %s\n", id, KEY_INFO[id] ? KEY_INFO[id] : "slot", ui_controls_key_name(code, name, sizeof(name)));
    menu_show_window(7, 1);
#ifdef __EMSCRIPTEN__
    g_ctl.swallowed_key = 1; // the page's own keydown listener runs after GLFW's
#endif
}

int ui_controls_take_swallowed_key(void) {
    const int s = g_ctl.swallowed_key;
    g_ctl.swallowed_key = 0;
    return s;
}

// ---------------------------------------------------------------- settings

UiInputSettings *ui_controls_settings(void) {
    return &g_ctl.settings;
}

float ui_controls_fps_fov(void) {
    const float deg = g_ctl.settings.fps_fov;
    return (deg >= 10.0f && deg <= 179.0f ? deg : 90.0f) * (float) M_PI / 180.0f;
}

int ui_controls_scroll_dir(void) {
    return g_ctl.settings.scroll_dir ? 1 : -1;
}

// showWeapon / showWeaponSec / showWeaponMel for the weapon in hand.
int ui_controls_weapon_visible(const Client *client) {
    const Player *me = client ? client->me : NULL;
    const Weapon *weapon = me ? me->weapon : NULL;
    if (!weapon) return 1;
    if (weapon->melee) return g_ctl.settings.show_weapon_mel;
    if (weapon->secondary) return g_ctl.settings.show_weapon_sec;
    return g_ctl.settings.show_weapon;
}

// shwChat.set + focusChat(): #chatList is shown for "All", only while typing for
// "On Focus", never for "Off".
void ui_controls_apply_chat_visibility(void) {
    HtmlElem list = html_dom_by_id("chatList");
    if (!list) return;
    const int mode = g_ctl.settings.shw_chat;
    if (mode == 1) html_dom_set_display(list, g_ctl.chat_focused ? "block" : "none");
    else html_dom_set_display(list, mode == 0 ? "block" : "none");
}

// ---------------------------------------------------------------- chat

static HtmlElem chat_input(void) {
    return html_dom_by_id("chatInput");
}

// window.focusChat(on): #chatInput's onfocus/onblur.
void ui_controls_chat_focus(const int focused) {
    g_ctl.chat_focused = !!focused;
    if (g_ctl.settings.shw_chat == 1) ui_controls_apply_chat_visibility();
}

int ui_text_input_focused(void) {
    if (g_ctl.chat_focused || ui_terminal_focused()) return 1;
    const HtmlElem el = html_dom_active_element();
    if (!el) return 0;
    char tag[16] = "", editable[16] = "";
    html_dom_tag(el, tag, sizeof(tag));
    if (!strcmp(tag, "input") || !strcmp(tag, "textarea") || !strcmp(tag, "select")) return 1;
    return html_dom_get_attr(el, "contenteditable", editable, sizeof(editable)) >= 0 && strcmp(editable, "false");
}

// Typing in #chatInput or in the developer console (main.c: no hotkeys, chars to the page).
int ui_controls_chat_focused(void) {
    return g_ctl.chat_focused || ui_console_is_open() || ui_terminal_focused();
}

static void chat_blur(void) {
    HtmlElem input = chat_input();
    if (input) html_dom_blur(input);
    ui_controls_chat_focus(0);
}

// client_chat_send (client.h, CLIENT_HAVE_CHAT_SEND) lives in the net layer; a weak
// reference keeps this file linking while that side is absent from a build.
#if defined(CLIENT_HAVE_CHAT_SEND) && (defined(__GNUC__) || defined(__clang__))
extern void client_chat_send(Client *, const char *) __attribute__((weak));
#define UI_CHAT_SEND_AVAILABLE() (client_chat_send != NULL)
#elif defined(CLIENT_HAVE_CHAT_SEND)
#define UI_CHAT_SEND_AVAILABLE() 1
#endif

// fp(msg): Krunker sends "ct" and shows the line when the server broadcasts it back.
static void chat_send(Client *client, const char *text) {
    if (!client) client = menu_ui()->client;
    if (ui_console_chat_command(client, text)) return; // the dev console mod's io.send wrapper: /bhop, singleplayer commands
#ifdef UI_CHAT_SEND_AVAILABLE
    if (client && UI_CHAT_SEND_AVAILABLE()) {
        client_chat_send(client, text); // net chat (client/src/net): the server's echo reaches the HUD
        return;
    }
#endif
    // No chat transport: echo the line locally.
    hud_ui_add_chat(client && client->menu_name[0] ? client->menu_name : "Guest", text, NULL);
    printf("[controls] chat (local echo): %s\n", text);
}

// pressButton(chatKey) for #chatInput: send + clear + blur when focused, else focus.
// Returns 1 when the key was used.
static int chat_key(Client *client) {
    HtmlElem input = chat_input();
    if (!input) return 0;
    if (g_ctl.chat_focused) {
        char text[CHAT_MAX] = "";
        if (html_dom_get_value(input, text, sizeof(text)) < 0) text[0] = 0;
        if (text[0]) {
            chat_send(client, text);
            html_dom_set_value(input, "");
        }
        chat_blur();
        return 1;
    }
    // chatInput.focus() only takes on a rendered input: #chatUI is shown in game.
    char display[16] = "";
    HtmlElem chat_ui = html_dom_by_id("chatUI");
    if (!chat_ui || (html_dom_get_style(chat_ui, "display", display, sizeof(display)) >= 0 && !strcmp(display, "none"))) return 0;
    html_dom_focus(input);
    ui_controls_chat_focus(1);
    return 1;
}

// The page saw Enter in #chatInput (the menu's key path / the web page's listener).
int ui_controls_chat_enter(Client *client) {
    if (!g_ctl.chat_focused) ui_controls_chat_focus(1); // the event came from the focused input
    return chat_key(client ? client : menu_ui()->client);
}

// ---------------------------------------------------------------- key state

static int code_down(const int code) {
    const int slot = key_slot(code);
    return slot >= 0 && g_ctl.keys[slot];
}

// CONTROLS.keys[code] for hard-coded keys (the spectator free cam's Q/E).
int ui_controls_code_down(const int code) {
    return code_down(code);
}

int ui_controls_down(const int id) {
    ensure_loaded();
    return valid_id(id) && code_down(g_ctl.binds[id]);
}

static int pressed(const int id) {
    const int slot = key_slot(g_ctl.binds[id]);
    return slot >= 0 && g_ctl.did_pressed[slot];
}

int ui_controls_scoreboard_held(void) {
    return !ui_text_input_focused() && ui_controls_down(UI_CONTROL_BOARD);
}

// controls.keys[voiceKey]: -1 undefined (resetKeys empties keys), else the last state N()
// recorded while the controls were enabled.
static int g_voice_key = -1;

// N(code, down): `controls.keys[keyCode] !== aj && keyCode == voiceKey` -> toggleRecord(aj),
// ahead of the `!controls.enabled` return (push to talk works over the menu too).
static void voice_key(Client *client, const int code, const int down) {
    if (code < 0 || !bind_is(UI_CONTROL_VOICE, code) || g_voice_key == down) return;
    voice_toggle_record(client ? client : menu_ui()->client, down);
}

void ui_controls_reset_keys(void) {
    g_voice_key = -1;
    memset(g_ctl.keys, 0, sizeof(g_ctl.keys));
    memset(g_ctl.did_pressed, 0, sizeof(g_ctl.did_pressed));
    g_ctl.shoot = g_ctl.aim = 0;
    g_ctl.pending_swap = 0;
    g_ctl.scroll_pressed = 0;
}

// One log line per control for hotkeys whose Krunker effect has no KrunkNative backend.
static void log_once(const int id, const char *what) {
    if (g_ctl.logged[id]) return;
    g_ctl.logged[id] = 1;
    printf("[controls] %s: not available in KrunkNative (no-op)\n", what);
}

static void flip_setting(const char *key) {
    if (!key || !*key) return;
    const cJSON *v = ui_setting_value(key);
    const int on = cJSON_IsTrue(v) || (cJSON_IsNumber(v) && cJSON_GetNumberValue(v) != 0.0) ||
        (cJSON_IsString(v) && cJSON_GetStringValue(v)[0] && strcmp(cJSON_GetStringValue(v), "false") != 0);
    const HtmlArg value = {HTML_ARG_BOOL, on ? 0.0 : 1.0, NULL};
    ui_settings_set(key, &value);
}

// window.pressButton(keyCode), the parts with an in-game effect.
static void press_button(Client *client, const int code) {
    if (bind_is(UI_CONTROL_PLAYER_LIST, code)) log_once(UI_CONTROL_PLAYER_LIST, "player list (window 23)");
    // sBoardKey: g6(1) -> hud_ui polls ui_controls_scoreboard_held()
    // window.Bf5: F free cam, H/0 objective cams, 1-9 players, R first/third person (raw keyCodes).
    if (client && spectate_press_key(client, code)) return;
    Player *me = client ? client->me : NULL;
    if (!me || !me->active) return;
    if (bind_is(UI_CONTROL_SPRAY, code)) { // io.send("int", 0): the authority places it (players.spray)
        const NetInteractPacket spray = {.action = 0};
        client_net_send(client, NET_PACKET_INTERACT, &spray, sizeof(spray));
    }
    if (bind_is(UI_CONTROL_INTERACT, code)) ui_terminal_interact(client, 0);     // io.send("int", 1)
    if (bind_is(UI_CONTROL_INTERACT_SEC, code)) ui_terminal_interact(client, 1); // io.send("int", 1, true)
    if (bind_is(UI_CONTROL_DROP, code)) log_once(UI_CONTROL_DROP, "drop (int 2)");
    if (bind_is(UI_CONTROL_INSPECT, code)) player_inspect_weapon(me); // Game.players.wInspect(player)
    for (int i = 0; i < 5; i++) {
        if (bind_is(UI_CONTROL_STREAK0 + i, code)) log_once(UI_CONTROL_STREAK0 + i, "killstreak (k)");
    }
    if (bind_is(UI_CONTROL_WEAPON_VIS, code) && me->weapon) {
        if (me->weapon->melee) flip_setting("showWeaponMel");
        else if (me->weapon->secondary) flip_setting("showWeaponSec");
        else flip_setting("showWeapon");
    }
    for (int i = 0; i < TOGGLE_COUNT; i++) {
        if (bind_is(TOGGLE_FIRST + i, code)) flip_setting(g_ctl.toggle_sets[i]); // gG(i)
    }
    // io.send("prmA", i): 0 noclip, 1 god, 2 kill, 3 smite. The server (or the offline game)
    // decides and answers NET_CHEAT_NOCLIP / chat lines / notify; nothing flips locally, so the
    // prediction and the authority always simulate the same flags. KrunkNative's N key
    // (78, both GLFW and DOM keyCode) is an extra premiumKeys[0].
    for (int i = 0; i < 4; i++) {
        if (!bind_is(UI_CONTROL_PREMIUM0 + i, code) && !(i == 0 && code == 78)) continue;
        const uint8_t index = (uint8_t) i;
        client_net_send(client, NET_PACKET_PREMIUM_ACTION, &index, sizeof(index));
    }
}

// CONTROLS N(code, 1) while the game owns the input.
static void press_code(Client *client, const int code) {
    const int slot = key_slot(code);
    if (slot < 0 || g_ctl.keys[slot]) return;
    if (bind_is(UI_CONTROL_VOICE, code)) g_voice_key = 1;
    g_ctl.keys[slot] = 1;
    g_ctl.did_pressed[slot] = 1;
    if (bind_is(UI_CONTROL_SWAP, code)) g_ctl.pending_swap = 1;
    else if (bind_is(UI_CONTROL_MELEE, code)) g_ctl.pending_swap = 2;
    else if (bind_is(UI_CONTROL_PRIMARY, code)) g_ctl.pending_swap = 3;
    else if (bind_is(UI_CONTROL_EQUIP, code)) g_ctl.pending_swap = 4;
    press_button(client, code);
}

// CONTROLS N(code, 0) (+ releaseButton: the leaderboard is polled)
static void release_code(const int code) {
    const int slot = key_slot(code);
    if (bind_is(UI_CONTROL_VOICE, code)) g_voice_key = 0;
    if (slot >= 0) g_ctl.keys[slot] = 0;
}

// ---------------------------------------------------------------- GLFW events

int ui_controls_key_event(Client *client, const int key, const int scancode, const int action, const int mods) {
    ensure_loaded();
    const int code = ui_controls_dom_key(key);
    const int press = action == GLFW_PRESS;
    const int menu = html_ui_is_menu_open();
    g_ctl.swallowed_key = 0; // only ever about the key event being dispatched right now

    if (press && ui_controls_capturing()) {
        // Krunker's keydown hands the chat key to pressButton (it focuses the chat)
        // instead of N(), so it never reaches the capture.
        if (code >= 0 && !bind_is(UI_CONTROL_CHAT, code)) capture_bind(code);
        return 1;
    }

    if (ui_console_key_event(client, key, scancode, action, mods)) return 1; // dev console + mods.ts keys (ui_console.c)
    if (ui_terminal_focused()) return ui_terminal_key_event(client, key, scancode, action, mods, code >= 0 && bind_is(UI_CONTROL_CHAT, code));

    if (g_ctl.chat_focused) {
        if (press && code >= 0 && bind_is(UI_CONTROL_CHAT, code)) return chat_key(client);
        if (menu) return 0; // the page edits the focused input
        if (action == GLFW_RELEASE) release_code(code); // keyups still reach N()
        if (press && key == GLFW_KEY_ESCAPE) {
            chat_blur();
            return 0; // main.c opens the menu (Krunker: the pointer lock is lost)
        }
        html_ui_key(key, scancode, action, mods); // typing goes to #chatInput, not the player
        return 1;
    }

    // pressButton(27): N() lets Escape through while the controls are disabled, and
    // pressButton toggles the end-screen results <-> the menu (UI.toggleEndScreen).
    if (press && code == 27 && hud_ui_toggle_end_screen(client)) {
#ifdef __EMSCRIPTEN__
        g_ctl.swallowed_key = 1; // the page's own Escape handling (ui_handlers.c) must not run too
#endif
        return 1;
    }

    if (action != GLFW_REPEAT) voice_key(client, code, press);
    if (menu || action == GLFW_REPEAT) return 0;
    if (action == GLFW_PRESS && ui_text_input_focused()) return 0; // a field (any page) keeps the key: no game bind
    if (press && code >= 0 && bind_is(UI_CONTROL_CHAT, code) && chat_key(client)) return 1;
    if (press) press_code(client, code);
    else release_code(code);
    return 0;
}

int ui_controls_mouse_event(Client *client, const int button, const int action, const int in_game) {
    ensure_loaded();
    const int code = mouse_code(button);
    if (!in_game) {
        // #menuWindow mousedown: middle and side buttons are bindable (left/right click the UI)
        const int bindable = code == KEY_MOUSE_BASE + 2 || code > KEY_MOUSE_BASE + 3;
        if (action == GLFW_PRESS && bindable && ui_controls_capturing()) {
            capture_bind(code);
            return 1;
        }
        return 0;
    }
    if (action == GLFW_PRESS && ui_console_is_open()) return 1; // the console frees the pointer: no shot
    if (ui_mod_keys_ctrl_tp_mouse(client, button, action)) return 1; // mods.ts Ctrl TP takes the left click
    if (action == GLFW_PRESS) {
        if (g_ctl.chat_focused) chat_blur(); // a click on the canvas leaves the chat
        // window.Bf5: left = spectMode(-1) (Prev [LM]), right = spectMode(1) (Next [RM])
        if (client && (button == GLFW_MOUSE_BUTTON_LEFT || button == GLFW_MOUSE_BUTTON_RIGHT)) {
            spectate_mouse_press(client, button == GLFW_MOUSE_BUTTON_LEFT ? 1 : 3);
        }
        if (button == GLFW_MOUSE_BUTTON_RIGHT) g_ctl.aim = 1;
        else if (button == GLFW_MOUSE_BUTTON_LEFT) g_ctl.shoot = 1;
        else press_code(client, code);
    } else if (action == GLFW_RELEASE) {
        gamepad_set_active(0); // canvas mouseup: aa(event) with inputType != "cntrlInput"
        if (button == GLFW_MOUSE_BUTTON_RIGHT) g_ctl.aim = 0;
        else if (button == GLFW_MOUSE_BUTTON_LEFT) g_ctl.shoot = 0;
        else release_code(code);
    }
    return 1;
}

int ui_controls_scroll_event(Client *client, const double dy, const int in_game) {
    ensure_loaded();
    if (!dy) return 0;
    if (!in_game) {
        if (!ui_controls_capturing()) return 0;
        capture_bind(KEY_SCROLL); // #menuWindow wheel while waiting for a key
        return 1;
    }
    // window.Bf5: the wheel sets the free-cam speed / follow distance (then Q() as usual).
    if (client) spectate_scroll(client, dy > 0.0);
    // v3.1.1 offline mod (mods.ts installScrollWheelPatch / patchScrollInput): with a live
    // player, the controls enabled, no "Press any Key" and no spectating, the wheel always
    // swaps weapons (scrollToSwap, scrollDirM) and its key state is cleared, so a SCROLL
    // bind never fires. main.c turns the kept scroll_delta into the swap.
    if (client && client->me && client->me->active && ui_controls_enabled(client) && !ui_controls_capturing() && !spectate_active()) return 0;
    // Q(null, 20000): the wheel is a key for one input frame; it swaps weapons only
    // while no bind uses it (bMoQ7).
    press_code(client, KEY_SCROLL);
    g_ctl.scroll_pressed = 1;
    return g_ctl.scroll_bound;
}

// ---------------------------------------------------------------- controller

// The controls.js entry points the gamepad module (client/src/gamepad.c) drives: Q()/U()
// with a bind's keyCode, fakeMouse() as "cntrlInput", toggle(). They act on the same key
// state, shoot flag and look sums as the keyboard and mouse.

UiControllerInput *ui_controls_cntrl_input(void) {
    return &g_ctl.cntrl;
}

// CONTROLS.enabled: the pointer is locked to the game (menu closed, no end-screen results).
int ui_controls_enabled(const Client *client) {
    return client && client->mouse_state.locked && !html_ui_is_menu_open() && !hud_ui_end_screen_interactive();
}

// CONTROLS N(code, down) as reached from Q()/U(): a pending "Press any Key" takes the
// code; otherwise nothing happens while the game does not own the input (keyCode 27
// aside), and the key goes down/up like a keyboard key.
static void controller_n(Client *client, const int code, const int down) {
    const int slot = key_slot(code);
    if (down && !(slot >= 0 && g_ctl.keys[slot]) && ui_controls_capturing()) {
        capture_bind(code);
        return;
    }
    voice_key(client, code, down);
    if (!ui_controls_enabled(client) && code != 27) return;
    if (down) press_code(client, code);
    else release_code(code);
}

// Q(null, binds[id].val) (down) / U(null, binds[id].val) (up)
void ui_controls_controller_key(Client *client, const int id, const int down) {
    ensure_loaded();
    if (!valid_id(id)) return;
    const int code = g_ctl.binds[id];
    if (down) {
        const int typing = ui_text_input_focused();
        if (bind_is(UI_CONTROL_CHAT, code)) {
            // window.pressButton(chatKey): sends / blurs #chatInput; not from another field
            if (!typing || g_ctl.chat_focused) chat_key(client ? client : menu_ui()->client);
            return;
        }
        if (typing) return; // document.activeElement == chatInput (KrunkNative: any text field)
    }
    controller_n(client, code, down);
}

// CONTROLS.toggle(on). this.locked (CONTROLS.disable() once the end screen shows its
// results) / masterLock (no map yet): only UI.toggleEndScreen() (hud_ui.c). A window
// open over the menu closes; otherwise the pointer lock is requested (play) or
// released (menu).
void ui_controls_toggle(Client *client, const int on) {
    if (!client) return;
    if (!client->game.ready || hud_ui_end_screen_interactive()) {
        if (on) hud_ui_toggle_end_screen(client); // UI.hasEndScreen && on
        return;
    }
    // Title animation (controls not locked yet): losing the lock shows no menu, hideUI() set
    // UI.active = false (toggleControlUI); a click still takes the pointer back.
    if (!on && hud_ui_end_screen_up()) return;
    if (html_ui_is_menu_open() && menu_window_is_open()) {
        if (on) menu_toggle_window(0); // UI.toggleWindow(false, gamepad.active)
        return;
    }
    if (on) {
        if (!ui_controls_enabled(client)) menu_play(); // requestPointerLock -> enterGame()
    } else if (ui_controls_enabled(client)) {
        client_set_menu_open(client, 1);  // document.exitPointerLock()
    }
}

// CONTROLS.fakeMouse(type, x, y, 0, "cntrlInput"): 0 = a9 (mousedown, which 0),
// 1 = aa (mouseup), 2 = a8 (mousemove by x/y).
void ui_controls_fake_mouse(Client *client, const int type, const float dx, const float dy) {
    if (type == 0) {
        // a9: `if (ak && gamepad.active) gamepad.active = false` (ak = "cntrlInput")
        gamepad_set_active(0);
        if (!client || !client->game.ready) return; // masterLock
        if (!ui_controls_enabled(client)) {
            ui_controls_toggle(client, 1);
            return;
        }
        if (g_ctl.chat_focused) chat_blur();
        g_ctl.shoot = 1;                    // which 0: the default branch -> UCUz1 = 1
        spectate_mouse_press(client, 1);    // window.Bf5: spectMode(-1)
    } else if (type == 1) {
        if (!hud_ui_end_screen_interactive()) g_ctl.shoot = 0; // !locked: UCUz1 = 0
    } else if (type == 2) {
        if (!ui_controls_enabled(client)) return;
        // window.noMouseInp: the first-person spectator view takes no look input. The
        // other spectator cams turn with these sums (spectate_tick -> ui_controls_fill_input).
        if (spectate_active() && spectate_first_person() && spectate_target_uid() >= 0) return;
        g_ctl.input_cntrl = 1;              // inputType = "cntrlInput"
        g_ctl.look_x += dx;                 // a4 += movementX
        g_ctl.look_y += dy;                 // a5 += movementY
    }
}

int ui_controls_input_is_cntrl(void) {
    return g_ctl.input_cntrl;
}

// The end of updateMouseInp(): a4 = a5 = 0, whether or not the frame used them.
void ui_controls_look_frame_end(void) {
    g_ctl.look_x = g_ctl.look_y = 0.0f;
}

// #mobileUI's mobileShoot / mobileAimShoot (ui_mobile.c): UCUz1 directly.
void ui_controls_touch_shoot(const int on) {
    g_ctl.shoot = on;
}

void ui_controls_set_speed_limit(const float limit) {
    g_ctl.speed_lmt = limit;
}

// CONTROLS.speedLmt; the input frame sends `1 - speedLmt.round(1)` (b9[12], 95.js
// procInputs: accel *= limitMM(1 - input[12], 0, 1)).
float ui_controls_speed_limit(void) {
    return g_ctl.speed_lmt;
}

// ---------------------------------------------------------------- per-frame input

// aba6 -> Input.swap, Krunker's input[11] codes as is: 1 swapSecondary, 2 swapMelee,
// 3 primary (slot 0), 4 swapEquipment (player_proc_input).
static int input_swap(const Player *me) {
    (void) me;
    return g_ctl.pending_swap >= 1 && g_ctl.pending_swap <= 4 ? g_ctl.pending_swap : 0;
}

// The input-frame half of Krunker's loop (b9): look delta (CONTROLS.updateMouseInp:
// mouseSens * sens * camera.fov / fov, aim sensitivities while the right button is
// held, invertY), move direction from the move binds, and the bound buttons.
void ui_controls_fill_input(Client *client, Input *input, const float dx, const float dy) {
    ensure_loaded();
    const ClientSettings *s = &menu_ui()->settings;
    const float zoom = client && client->camera.zoom > 0.0f ? client->camera.zoom : 1.0f;
    const float base = game_constants.mouse_sensitivity / zoom;
    // a6 = this[inputType || "kbmInput"]: a mouse movement since the last gamepad loop
    // makes it kbmInput again; the right stick's fakeMouse(2) sums (a4/a5) are scaled
    // together with the mouse movement by whichever input type moved last.
    if (dx != 0.0f || dy != 0.0f) g_ctl.input_cntrl = 0;
    const float look_x = dx + g_ctl.look_x;
    const float look_y = dy + g_ctl.look_y;
    g_ctl.look_x = g_ctl.look_y = 0.0f;
    const UiControllerInput *c = &g_ctl.cntrl;
    const float sens_x = g_ctl.input_cntrl ? (g_ctl.aim ? c->aim_x : c->sens_x) : (g_ctl.aim ? s->aim_sensitivity_x : s->sensitivity_x);
    const float sens_y = g_ctl.input_cntrl ? (g_ctl.aim ? c->aim_y : c->sens_y) : (g_ctl.aim ? s->aim_sensitivity_y : s->sensitivity_y);
    const int invert = g_ctl.input_cntrl ? c->invert : s->invert_y;
    input->x_dir -= look_y * base * sens_y * (invert ? -1.0f : 1.0f);
    input->y_dir -= look_x * base * sens_x;
    float touch_x, touch_y; // fakeMouse(2, dx, dy, 0, "mblInput"): mobile sensitivities (ui_mobile.c)
    if (ui_mobile_take_look(g_ctl.aim, &touch_x, &touch_y)) {
        input->x_dir -= touch_y * base;
        input->y_dir -= touch_x * base;
    }

    const int forward = ui_controls_down(UI_CONTROL_FORWARD);
    const int back = ui_controls_down(UI_CONTROL_BACKWARD);
    const int left = ui_controls_down(UI_CONTROL_LEFT);
    const int right = ui_controls_down(UI_CONTROL_RIGHT);
    input->move_dir = -1;
    if (forward ^ back) {
        input->move_dir += forward ? 1 : 5;
        if (left ^ right) input->move_dir += right ? (forward ? 1 : -1) : forward ? 7 : 1;
    } else if (left ^ right) {
        input->move_dir += right ? 3 : 7;
    }

    input->jump = pressed(UI_CONTROL_JUMP); // v3.1.1 sends jump on the press frame only (holding Space = one jump)
    input->crouch = ui_controls_down(UI_CONTROL_CROUCH);
    input->reload = ui_controls_down(UI_CONTROL_RELOAD);
    input->shoot = g_ctl.shoot || ui_controls_down(UI_CONTROL_SHOOT);
    input->scope = g_ctl.aim || ui_controls_down(UI_CONTROL_AIM);
    input->swap = input_swap(client ? client->me : NULL);
    input->slowdown = 1.0f - roundf(g_ctl.speed_lmt * 10.0f) / 10.0f; // b9[12]: 1 - speedLmt.round(1)
    ui_bhop_fill_input(client, input); // v3.1.1 mod patchInputs: bhop jump/crouch slots (ui_bhop.c)
}

// After the input frame: didPressed, aba6 and the wheel "key" are one-frame events.
void ui_controls_end_frame(void) {
    memset(g_ctl.did_pressed, 0, sizeof(g_ctl.did_pressed));
    g_ctl.pending_swap = 0;
    if (g_ctl.scroll_pressed) {
        release_code(KEY_SCROLL);
        g_ctl.scroll_pressed = 0;
    }
}
