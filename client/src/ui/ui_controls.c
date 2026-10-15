// Krunker v7.1.4's keybinds (CONTROLS, src/libs/controls.js + controlBind.js) and the
// in-game key/mouse routing built on them:
//   - the binding table: every v7 ControlBind (a key and an alternate key, DOM keyCodes,
//     mouse buttons 10000 + event.which, the wheel 20000) under v7's localStorage keys
//     (cont_<name>, cont_<name>_alt, cont_27_set.. for the Toggle rows, cont_43_set.. for
//     the quick chat messages) through html_storage_*, mapped onto KrunkNative's bind ids
//     (UI_CONTROL_*) for the game side; v7 binds without a KrunkNative action yet are
//     stored and shown only;
//   - the window markup (windows[6].gen: 5 tabs, collapsible groups, reset / unbind per
//     key cell, Toggle selects, Message inputs, Trigger selects), changeCont / unbindCont /
//     resetCont / changeContSet / changeContMsg and the "Press any Key" capture
//     (CONTROLS N(): the next key, mouse button or wheel turn becomes the bind);
//   - the bound-key state the game samples every frame (CONTROLS.keys / didPressed /
//     UCUz1 / DNCv5 / aba6 / wDG6) and the pressButton()/releaseButton() hotkeys;
//   - chat focus (#chatInput, the chat key opens/sends) and the input settings the
//     engine applies (sensitivity, invert, fpsFOV, scrollDir, weapon visibility).
// All of it is shared by the native and web clients: main.c feeds GLFW events (the
// emscripten GLFW port reports DOM keys as GLFW keys) and the page's focusChat events.
#include <ui_logic.h>
#include <render_settings.h> // showMessages (ui_controls_chat_text_allowed)
#include <html_ui.h>
#include <spectate.h> // spectator keys (pressButton's window.Bf5 block, client/src/spectate.c)
#include <gamepad.h>  // Gamepad.active (controller: client/src/gamepad.c)
#include <ui_mobile.h> // touch look (#mobileUI, client/src/ui/ui_mobile.c)
#include <console.h>   // the developer console takes the keyboard while open (ui_console.c)
#include <ui_interact.h> // v7 interaction: "int" / "intSec" type, uid, "drpw", "cnfm" (ui_interact.c)
#include <voice_chat.h> // voiceKey -> toggleRecord (client/src/voice.c)
#include <ctype.h>
#include <hud_social.h> // WP-E: spray / spray wheel, taunts, quick chat (hud_social.c)
#include <sprays.h>     // the spray wheel takes the mouse wheel
#include <third_person.h> // the Third Person bind
#include <app_mode.h>   // app_reload: the Reload Game / Reconnect keys on the web
#include <game_reload.h> // the Reload Game key natively
#include <object_manager.h> // the Object Manager's Undo / Remove binds
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

// UTILS.keyboardMap (v7.1.4 src/libs/utils.js:5): DOM keyCode -> name, 223 entries (NULL past
// them: getKeyName gives "undefined"). Byte-exact, tabs included ("F24\t\t", "TILDE\t", ...).
// v7 differs from v3.1.1's table from keyCode 136 on (v3 padded F24 with empty names; 186-191 are
// ";", "=", ",", "-", ".", "/" in v7).
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
    "F15", "F16", "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24\t\t", "NUM_LOCK", "SCROLL_LOCK",
    "WIN_OEM_FJ_JISHO", "WIN_OEM_FJ_MASSHOU", "WIN_OEM_FJ_TOUROKU", "WIN_OEM_FJ_LOYA", "WIN_OEM_FJ_ROYA\t\t", "",
    "CIRCUMFLEX", "EXCLAMATION", "DOUBLE_QUOTE", "HASH", "DOLLAR", "PERCENT", "AMPERSAND", "UNDERSCORE",
    "OPEN_PAREN", "CLOSE_PAREN", "ASTERISK", "PLUS", "PIPE", "HYPHEN_MINUS", "OPEN_CURLY_BRACKET",
    "CLOSE_CURLY_BRACKET", "TILDE\t", "VOLUME_MUTE", "VOLUME_DOWN", "VOLUME_UP", "", "", ";", "=", ",", "MINUS",
    "PERIOD", "SLASH", "BACK_QUOTE\t\t\t\t\t\t", "", "", "[", "\\", "]", "QUOTE", "", "META", "ALTGR", "",
    "WIN_ICO_HELP", "WIN_ICO_00", "", ";", "=", ",", "-", ".", "/", "~", "WIN_OEM_PA3", "WIN_OEM_WSCTRL",
    "WIN_OEM_CUSEL", "WIN_OEM_ATTN", "WIN_OEM_FINISH", "WIN_OEM_COPY", "WIN_OEM_AUTO", "WIN_OEM_ENLW",
    "WIN_OEM_BACKTAB", "ATTN", "CRSEL", "EXSEL", "EREOF", "PLAY", "ZOOM", "", "PA1", "WIN_OEM_CLEAR", "", "", "",
    "", "", "", "", "", "[", "\\", "]", "'", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
};
// The `toggle: true` settings of v10.0.2's SETTINGS.list (settings order) offered by the
// Toggle rows: value, "name [category header]" (windows[6].gen; ui/windows/controls-1.html).
// v10 also lists the hidden per-weapon clones (scrollDir_0, ...); KrunkNative has no per-weapon
// settings yet, so they are left out.
static const char *const TOGGLE_SETTINGS[][2] = {
    {"oldBrowser", "Legacy Browser [Server Browser & Matchmaking]"},
    {"defaultRegionOnly", "Default Region Only [Server Browser & Matchmaking]"},
    {"kpdVision", "KPD Vision [KPD]"},
    {"webgpu", "WebGPU rendering [Performance]"},
    {"antiAlias", "Antialiasing [Performance]"},
    {"lowSpec", "Low Spec [Performance]"},
    {"noTex", "No Textures [Performance]"},
    {"mapDet", "Map Details [Performance]"},
    {"particles", "Particles [Performance]"},
    {"shadows", "Shadows [Performance]"},
    {"shadowsDynamic", "Dynamic Shadows [Performance]"},
    {"ambientShading", "Old Shading (When Possible) [Performance]"},
    {"showTrails", "Bullet Trails [Performance]"},
    {"yourTrails", "Your Trails [Performance]"},
    {"muzzleFlash", "Show Muzzle Flash [Performance]"},
    {"bulletCasings", "Show Bullet Casings [Performance]"},
    {"impactHoles", "Show Bullet Impact Holes [Performance]"},
    {"sniperFlap", "Show Sniper Flap [Performance]"},
    {"sniperFlapAnim", "Sniper Flap Animation [Performance]"},
    {"textureAnim", "Texture Animations [Performance]"},
    {"objectAnim", "Object Animations [Performance]"},
    {"noPaintAnim", "Disable Animated Paints [Performance]"},
    {"screenShake", "Screen Shake [Performance]"},
    {"weaponShine", "Weapons Shine [Performance]"},
    {"showExplo", "Show Explosions [Performance]"},
    {"showUI", "Show UI [Interface]"},
    {"oldScoreboard", "Use Old Scoreboard [Interface]"},
    {"disableRarityAnim", "Disable Rarity Animations [Interface]"},
    {"showPfp", "Show Profile Pictures [Interface]"},
    {"dynamicHP", "Dynamic HP Bars [Interface]"},
    {"showHitInd", "Show Hit Indicators [Interface]"},
    {"showDMG", "Show Damage [Interface]"},
    {"showKills", "Show Kill Feed [Interface]"},
    {"showKillC", "Show Kill Counter [Interface]"},
    {"showDeaths", "Show Death Counter [Interface]"},
    {"showKD", "Show K/D Counter [Interface]"},
    {"showScore", "Show Score Counter [Interface]"},
    {"showStreak", "Show Streak Counter [Interface]"},
    {"showPing", "Show Ping [Interface]"},
    {"showFPS", "Show FPS [Interface]"},
    {"showSpeed", "Show Movement Speed [Interface]"},
    {"healthNum", "Nametag Health Number [Interface]"},
    {"hideNonTrade", "Hide Non-Tradable Skins [Interface]"},
    {"showMinimap", "Show Minimap [Minimap (Radar)]"},
    {"showMedals", "Show Medals [Medals]"},
    {"classicMedals", "Classic Medals [Medals]"},
    {"playMedals", "Play Medal Sounds [Medals]"},
    {"chatFilter", "Profanity Filter [Chat]"},
    {"showMessages", "Show Player Messages [Chat]"},
    {"showUnboxings", "Show Unboxings [Chat]"},
    {"showFindings", "Show Findings [Chat]"},
    {"chatTextOutline", "Chat Text Outline [Chat]"},
    {"crosshairUseOpacity", "Use Custom Opacity [Crosshair]"},
    {"crosshairAlways", "Always Show [Crosshair]"},
    {"crosshairDot", "Dot [Crosshair]"},
    {"crosshairUseOpacityM", "Use Custom Opacity [Crosshair (Throwing)]"},
    {"crosshairAlwaysM", "Always Show [Crosshair (Throwing)]"},
    {"crosshairDotM", "Dot [Crosshair (Throwing)]"},
    {"hitm", "Show [Hitmarker]"},
    {"scrollDir", "Scroll Direction [Gameplay]"},
    {"invertY", "Invert Y-Axis [Gameplay]"},
    {"challMode", "Challenge Mode [Gameplay]"},
    {"disableCons", "Disable Controllers [Controller]"},
    {"muteSpec", "Voice Mute Spectators [Audio]"},
    {"streamMode", "Streamer Mode [Gameplay Privacy]"},
    {"anonMode", "Anonymous Mode [Gameplay Privacy]"},
    {"showRegion", "Show Region Info [Gameplay Privacy]"},
    {"showServerTimer", "Show Server Timers [Gameplay Privacy]"},
    {"verifiedBadge", "Show Verified Badge [Gameplay Privacy]"},
    {"premiumBadge", "Show Premium Badge [Gameplay Privacy]"},
    {"customBadge", "Show Custom Badge [Gameplay Privacy]"},
    {"rankBadge", "Show Ranked Badge [Gameplay Privacy]"},
    {"toggleADS", "Toggle Weapon ADS [View Model]"},
    {"aimAnim", "Weapon Aim Animation [View Model]"},
    {"hideADS", "Hide Weapon on ADS [View Model]"},
    {"showHands", "Show Hands [View Model]"},
    {"showWeapon", "Show Primary [View Model]"},
    {"showWeaponSec", "Show Secondary [View Model]"},
    {"showWeaponMel", "Show Melee [View Model]"},
    {"leftHand", "Left Handed [View Model]"},
    {"roundArms", "Rounded Arms [View Model]"},
    {"wallLean", "Wall Slide/Jump Lean [View Model]"},
    {"showPScore", "Show Popup Score [Editing]"},
    {"scoreShadow", "Popup Score Shadow [Editing]"},
    {"progShadow", "Progress Bar Shadow [Editing]"},
    {"progMShadow", "Charge Bar Shadow [Editing]"},
    {"bulletTracers", "Bullet Tracers [Editing]"},
    {"scopeBorders", "Scope Borders [Customization]"},
    {"useDamageOverlay", "Use Damage Overlay [Customization]"},
    {"killFx", "Particle Effect on Kill [Customization]"},
    {"jumpScare", "Enable Jump Scare [Customization]"},
};

// v7.1.4's CONTROLS.binds (src/libs/controls.js): every ControlBind(name, def, altDef), with
// the bind / array index the controls window addresses it by and the KrunkNative bind id
// the game side acts on (-1: stored and shown, not acted on yet). Storage keys are v7's:
// cont_<name> and cont_<name>_alt (shoot/aim are shoot1Key/aim1Key: v3.1.1's shootKey /
// aimKey values do not carry over, as in v7).
typedef struct {
    const char *bind;  // binds.<bind>
    int index;         // array element, -1 scalar
    const char *name;  // ControlBind name (storage key cont_<name>)
    int def, alt_def;
    int kn_id;         // UI_CONTROL_*, -1 none
} V7Bind;

static const V7Bind V7_BINDS[] = {
    {"primary", -1, "primKey", 84, -1, UI_CONTROL_PRIMARY},
    {"reload", -1, "reloadKey", 82, -1, UI_CONTROL_RELOAD},
    {"jump", -1, "jumpKey", 32, -1, UI_CONTROL_JUMP},
    {"playerList", -1, "pListKey", 18, -1, UI_CONTROL_PLAYER_LIST},
    {"scoreBoard", -1, "sBoardKey", 9, -1, UI_CONTROL_BOARD},
    {"interact", -1, "interactKey", 71, -1, UI_CONTROL_INTERACT},
    {"confirm", -1, "confirmKey", 75, -1, -1},
    {"secondaryInteract", -1, "interactSecKey", 72, -1, UI_CONTROL_INTERACT_SEC},
    {"reset", -1, "resetKey", 66, -1, UI_CONTROL_RESET},
    {"resetLast", -1, "resetLastKey", 78, -1, UI_CONTROL_RESET_LAST},
    {"spray", -1, "sprayKey", 70, -1, UI_CONTROL_SPRAY},
    {"sprayWheel", -1, "sprayWheelKey", 70, -1, -1},
    {"inspect", -1, "inspKey", 88, -1, UI_CONTROL_INSPECT},
    {"swap", -1, "swapKey", 69, -1, UI_CONTROL_SWAP},
    {"shoot", -1, "shoot1Key", 10001, 220, UI_CONTROL_SHOOT},
    {"aim", -1, "aim1Key", 10003, 221, UI_CONTROL_AIM},
    {"crouch", -1, "crouchKey", 16, -1, UI_CONTROL_CROUCH},
    {"melee", -1, "meleeKey", 81, -1, UI_CONTROL_MELEE},
    {"equip", -1, "equipKey", 67, -1, UI_CONTROL_EQUIP},
    {"chat", -1, "chatKey", 13, -1, UI_CONTROL_CHAT},
    {"voiceChat", -1, "voiceKey", 86, -1, UI_CONTROL_VOICE},
    {"drop", -1, "dropKey", 90, -1, UI_CONTROL_DROP},
    {"wepVis", -1, "wepVisKey", -1, -1, UI_CONTROL_WEAPON_VIS},
    {"kickVoteY", -1, "kickVoteYKey", 49, -1, -1},
    {"kickVoteN", -1, "kickVoteNKey", 50, -1, -1},
    {"specFree", -1, "specFreeKey", 70, -1, -1},
    {"specObj", -1, "specObjKey", 72, -1, -1},
    {"specFirst", -1, "specFirstKey", 82, -1, -1},
    {"specNames", -1, "specNamesKey", 77, -1, -1},
    {"specMiniMap", -1, "specMiniMap", 189, -1, -1},
    {"kpdVoteY", -1, "kpdVoteYKey", 89, -1, -1},
    {"kpdVoteN", -1, "kpdVoteNKey", 78, -1, -1},
    {"specFocus", -1, "specFocusKey", 190, -1, -1},
    {"kpdVision", -1, "kpdVisionKey", 187, -1, -1},
    {"hidePlayers", -1, "hidePlayersKey", -1, -1, -1},
    {"prop", -1, "propKey", 80, -1, -1},
    {"propRand", -1, "propRandKey", 77, -1, -1},
    {"propRot", -1, "propRotKey", 82, -1, -1},
    {"propRotR", -1, "propRotRKey", 78, -1, -1},
    {"noclipSB", -1, "sandboxNoclip", -1, -1, -1},
    {"godModeSB", -1, "sandboxGodMode", -1, -1, -1},
    {"smiteSB", -1, "sandboxSmite", -1, -1, -1},         // v10.0.2
    {"unlimitedSB", -1, "sandboxUnlimited", -1, -1, -1}, // v10.0.2
    {"killSB", -1, "sandboxKill", -1, -1, -1},
    {"spawnBot", -1, "sandboxSpawnBot", 107, -1, -1},
    {"clearBots", -1, "sandboxClearBots", 109, -1, -1},
    // v10.0.2: Ping Position (ranked / comp only; M2 = the middle button)
    {"markPosition", -1, "markPositionKey", 10002, -1, -1},
    // KrunkNative additions (not v7.1.4 ControlBinds): Reload Game, F5 (keyCode 116) by default: v7
    // has no such bind, the browser's F5 reloads the page; natively the client relaunches into the
    // same game (game_reload.h; the console's `reload` is the in-place reload), the web build still
    // reloads the page (reload_key).
    // Reconnect (console `reconnect`) had F5 before and is unbound by default now.
    {"gameReload", -1, "gameReloadKey", 116, -1, UI_CONTROL_GAME_RELOAD},
    {"reconnect", -1, "reconnectKey", -1, -1, UI_CONTROL_RECONNECT},
    // KrunkNative addition: the Object Manager, F1 (keyCode 112): toggles the spawn menu and the pointer.
    {"objectManager", -1, "objectManagerKey", 112, -1, UI_CONTROL_OBJECT_MANAGER},
    // KrunkNative additions: the Object Manager's Undo (U, 85) and Remove at the crosshair (Delete, 46)
    {"omUndo", -1, "omUndoKey", 85, -1, UI_CONTROL_OM_UNDO},
    {"omRemove", -1, "omRemoveKey", 46, -1, UI_CONTROL_OM_REMOVE},
    // the Physics Gun's rotate key: R (82) while carrying (reload is off then); its snap key E (69)
    // held with crouch (Shift+E): 45 degrees and the grid. E stays Secondary Weapon otherwise.
    {"omRotate", -1, "omRotateKey", 82, -1, UI_CONTROL_OM_ROTATE},
    {"omPhysSnap", -1, "omPhysSnapKey", 69, -1, UI_CONTROL_OM_PHYS_SNAP},
    // KrunkNative additions: the Tool Gun's grid step down / up (PageDown 34 / PageUp 33: v7's ] is aim's
    // alternate key), in game with a KN building gun in hand
    {"omSnapDown", -1, "omSnapDownKey", 34, -1, UI_CONTROL_OM_SNAP_DOWN},
    {"omSnapUp", -1, "omSnapUpKey", 33, -1, UI_CONTROL_OM_SNAP_UP},
    // KrunkNative addition: Third Person, unbound (no free v7 key; bind one or use the console)
    {"thirdPerson", -1, "thirdPersonKey", -1, -1, UI_CONTROL_THIRD_PERSON},
    // KrunkNative addition: the Tool Gun's Properties key (I, 73): the selection's fields in the window
    {"omProps", -1, "omPropsKey", 73, -1, UI_CONTROL_OM_PROPS},
    {"streak", 0, "18", 49, -1, UI_CONTROL_STREAK0},
    {"streak", 1, "19", 50, -1, UI_CONTROL_STREAK0 + 1},
    {"streak", 2, "20", 51, -1, UI_CONTROL_STREAK0 + 2},
    {"taunt", 0, "taunt0", 49, -1, -1},
    {"taunt", 1, "taunt1", 50, -1, -1},
    {"taunt", 2, "taunt2", 51, -1, -1},
    {"taunt", 3, "taunt3", 52, -1, -1},
    {"taunt", 4, "taunt4", 53, -1, -1},
    {"taunt", 5, "taunt5", 54, -1, -1},
    // v10.0.2: the Stalker team's taunts (io.send STALKER_TAUNT, i)
    {"stalkerTaunt", 0, "stalkerTaunt0", 49, -1, -1},
    {"stalkerTaunt", 1, "stalkerTaunt1", 50, -1, -1},
    {"stalkerTaunt", 2, "stalkerTaunt2", 51, -1, -1},
    {"stalkerTaunt", 3, "stalkerTaunt3", 52, -1, -1},
    {"stalkerTaunt", 4, "stalkerTaunt4", 53, -1, -1},
    {"stalkerTaunt", 5, "stalkerTaunt5", 54, -1, -1},
    {"move", 0, "0", 87, -1, UI_CONTROL_FORWARD},
    {"move", 1, "1", 83, -1, UI_CONTROL_BACKWARD},
    {"move", 2, "2", 65, -1, UI_CONTROL_LEFT},
    {"move", 3, "3", 68, -1, UI_CONTROL_RIGHT},
    {"toggle", 0, "27", -1, -1, UI_CONTROL_TOGGLE0},
    {"toggle", 1, "28", -1, -1, UI_CONTROL_TOGGLE0 + 1},
    {"toggle", 2, "29", -1, -1, UI_CONTROL_TOGGLE0 + 2},
    {"toggle", 3, "30", -1, -1, UI_CONTROL_TOGGLE0 + 3},
    {"toggle", 4, "31", -1, -1, UI_CONTROL_TOGGLE0 + 4},
    {"toggle", 5, "32", -1, -1, UI_CONTROL_TOGGLE0 + 5},
    {"premium", 0, "34", -1, -1, UI_CONTROL_PREMIUM0},
    {"premium", 1, "35", -1, -1, UI_CONTROL_PREMIUM0 + 1},
    {"premium", 2, "36", -1, -1, UI_CONTROL_PREMIUM0 + 2},
    {"premium", 3, "37", -1, -1, UI_CONTROL_PREMIUM0 + 3},
    {"message", 0, "43", -1, -1, -1},
    {"message", 1, "44", -1, -1, -1},
    {"message", 2, "45", -1, -1, -1},
    {"message", 3, "46", -1, -1, -1},
    {"message", 4, "47", -1, -1, -1},
    {"message", 5, "48", -1, -1, -1},
};
#define V7_BIND_COUNT ((int) (sizeof(V7_BINDS) / sizeof(V7_BINDS[0])))
#define MESSAGE_COUNT 6
#define MESSAGE_MAX 72 // config.chatMaxLength 70

// windows[6].tabs (index.js): tab name, then rows {i18n key, bind, index, list, group, list index}.
// `list` "toggleSets" = a Toggle <select> row, "chatMessages" = a quick chat Message input row.
typedef struct {
    const char *label;  // windows.controls.<key> (en)
    const char *bind;
    int index;
    int list;           // 0 none, 1 toggleSets, 2 chatMessages
    const char *group;
} ControlRow;

typedef struct {
    const char *name;
    const ControlRow *rows;
    int count;
} ControlTab;

static const ControlRow TAB_GENERAL[] = {
    {"Forward", "move", 0, 0, "Movement"}, {"Backward", "move", 1, 0, "Movement"}, {"Left", "move", 2, 0, "Movement"},
    {"Right", "move", 3, 0, "Movement"}, {"Jump", "jump", -1, 0, "Movement"}, {"Crouch", "crouch", -1, 0, "Movement"},
    {"Reload", "reload", -1, 0, "Weapon"}, {"Aim", "aim", -1, 0, "Weapon"}, {"Shoot", "shoot", -1, 0, "Weapon"},
    {"Primary Weapon", "primary", -1, 0, "Weapon"}, {"Melee Weapon", "melee", -1, 0, "Weapon"},
    {"Secondary Weapon", "swap", -1, 0, "Weapon"}, {"Equipment", "equip", -1, 0, "Weapon"},
    {"Inspect", "inspect", -1, 0, "Weapon"}, {"Spray", "spray", -1, 0, "Weapon"},
    {"Spray Wheel (Hold)", "sprayWheel", -1, 0, "Weapon"}, {"Ping Position (Ranked Only)", "markPosition", -1, 0, "Weapon"},
    {"Player List", "playerList", -1, 0, "Interface"},
    {"InGame Leaderboard", "scoreBoard", -1, 0, "Interface"}, {"Hide Players", "hidePlayers", -1, 0, "Players"},
    // KrunkNative additions, last so v7's rows keep their places (binds.reconnect / objectManager above)
    {"Reload Game", "gameReload", -1, 0, "Connection"},
    {"Reconnect", "reconnect", -1, 0, "Connection"},
    {"Object Manager", "objectManager", -1, 0, "Sandbox"},
    {"Object Manager Undo", "omUndo", -1, 0, "Sandbox"},
    {"Object Manager Remove", "omRemove", -1, 0, "Sandbox"},
    {"Physics Gun Rotate (held)", "omRotate", -1, 0, "Sandbox"},
    {"Physics Gun Snap (held with Crouch)", "omPhysSnap", -1, 0, "Sandbox"},
    {"Tool Gun Snap Down", "omSnapDown", -1, 0, "Sandbox"},
    {"Tool Gun Snap Up", "omSnapUp", -1, 0, "Sandbox"},
    {"Tool Gun Properties", "omProps", -1, 0, "Sandbox"},
    {"Third Person", "thirdPerson", -1, 0, "Camera"},
};
static const ControlRow TAB_CHAT[] = {
    {"Chat", "chat", -1, 0, "Interface & Audio"}, {"Voice", "voiceChat", -1, 0, "Interface & Audio"},
    {"Noclip", "premium", 0, 0, "Premium & Host"}, {"Godmode", "premium", 1, 0, "Premium & Host"},
    {"Kill", "premium", 2, 0, "Premium & Host"}, {"Smite", "premium", 3, 0, "Premium & Host"},
    {NULL, "message", 0, 2, "Chat Messages"}, {NULL, "message", 1, 2, "Chat Messages"}, {NULL, "message", 2, 2, "Chat Messages"},
    {NULL, "message", 3, 2, "Chat Messages"}, {NULL, "message", 4, 2, "Chat Messages"}, {NULL, "message", 5, 2, "Chat Messages"},
    {"Toggle Weapon Visibility", "wepVis", -1, 0, "Toggle Settings"},
    {NULL, "toggle", 0, 1, "Toggle Settings"}, {NULL, "toggle", 1, 1, "Toggle Settings"}, {NULL, "toggle", 2, 1, "Toggle Settings"},
    {NULL, "toggle", 3, 1, "Toggle Settings"}, {NULL, "toggle", 4, 1, "Toggle Settings"}, {NULL, "toggle", 5, 1, "Toggle Settings"},
};
static const ControlRow TAB_INTERACTION[] = {
    {"Interact", "interact", -1, 0, "Interaction"}, {"Secondary Interact", "secondaryInteract", -1, 0, "Interaction"},
    {"Drop", "drop", -1, 0, "Interaction"}, {"Confirm Interact", "confirm", -1, 0, "Interaction"},
    {"Streak Slot 1", "streak", 0, 0, "Killstreaks"}, {"Streak Slot 2", "streak", 1, 0, "Killstreaks"},
    {"Streak Slot 3", "streak", 2, 0, "Killstreaks"},
};
static const ControlRow TAB_SPECTATE[] = {
    {"Toggle Focus Player", "specFocus", -1, 0, "Spectating"}, {"Toggle Free Cam", "specFree", -1, 0, "Spectating"},
    {"Toggle Objective Cam", "specObj", -1, 0, "Spectating"}, {"Toggle First Person", "specFirst", -1, 0, "Spectating"},
    {"Toggle Forced Names", "specNames", -1, 0, "Spectating"}, {"Toggle Spectator Minimap", "specMiniMap", -1, 0, "Spectating"},
    {"KPD Vote [Yes]", "kpdVoteY", -1, 0, "KPD"}, {"KPD Vote [No]", "kpdVoteN", -1, 0, "KPD"},
    {"Toggle KPD Vision", "kpdVision", -1, 0, "KPD"}, {"Vote Kick [Yes]", "kickVoteY", -1, 0, "Vote"},
    {"Vote Kick [No]", "kickVoteN", -1, 0, "Vote"},
};
static const ControlRow TAB_MODES[] = {
    {"Open Prop Selection (Hold)", "prop", -1, 0, "Prop Hunt"}, {"Select Random Prop", "propRand", -1, 0, "Prop Hunt"},
    {"Toggle Rotation", "propRot", -1, 0, "Prop Hunt"}, {"Reset Rotation", "propRotR", -1, 0, "Prop Hunt"},
    {"Taunt (Psst)", "taunt", 0, 0, "Prop Hunt"}, {"Taunt (Whistle)", "taunt", 1, 0, "Prop Hunt"},
    {"Taunt (Fart)", "taunt", 2, 0, "Prop Hunt"}, {"Taunt (Meme)", "taunt", 3, 0, "Prop Hunt"},
    {"Taunt (WTF is This?)", "taunt", 4, 0, "Prop Hunt"}, {"Taunt (Right Now!)", "taunt", 5, 0, "Prop Hunt"},
    {"Taunt (Turn around)", "stalkerTaunt", 0, 0, "Stalker"}, {"Taunt (You are next)", "stalkerTaunt", 1, 0, "Stalker"},
    {"Taunt (Look up)", "stalkerTaunt", 2, 0, "Stalker"}, {"Taunt (Right here)", "stalkerTaunt", 3, 0, "Stalker"},
    {"Taunt (Over here)", "stalkerTaunt", 4, 0, "Stalker"}, {"Taunt (No escape)", "stalkerTaunt", 5, 0, "Stalker"},
    {"Reset Position (Parkour etc)", "reset", -1, 0, "Parkour & Race"},
    {"Reset To Last Checkpoint (Parkour etc)", "resetLast", -1, 0, "Parkour & Race"},
    {"Noclip", "noclipSB", -1, 0, "Sandbox (Test mode)"}, {"Godmode", "godModeSB", -1, 0, "Sandbox (Test mode)"},
    {"Smite", "smiteSB", -1, 0, "Sandbox (Test mode)"}, {"Unlimited Ammo", "unlimitedSB", -1, 0, "Sandbox (Test mode)"},
    {"Kill", "killSB", -1, 0, "Sandbox (Test mode)"}, {"Spawn Bot", "spawnBot", -1, 0, "Sandbox (Test mode)"},
    {"Clear Bots", "clearBots", -1, 0, "Sandbox (Test mode)"},
};
#define TAB(name, rows) {name, rows, (int) (sizeof(rows) / sizeof(rows[0]))}

// Localisation: v10's rows are i18n windows.controls.<key> values, which the page translation
// (i18n_translate_tree of the window) maps back to their keys; v10's tab and group names are
// English literals there. KrunkNative's own rows and groups take kn.controls.<bind> /
// kn.controls.group.<id> (named here for tools/i18n-import/i18n_lint.py):
//   I18N("kn.controls.gameReload", "Reload Game")  I18N("kn.controls.reconnect", "Reconnect")
//   I18N("kn.controls.objectManager", "Object Manager")  I18N("kn.controls.omUndo", "Object Manager Undo")
//   I18N("kn.controls.omRemove", "Object Manager Remove")  I18N("kn.controls.omRotate", "Physics Gun Rotate (held)")
//   I18N("kn.controls.omPhysSnap", "Physics Gun Snap (held with Crouch)")  I18N("kn.controls.omSnapDown", "Tool Gun Snap Down")
//   I18N("kn.controls.omSnapUp", "Tool Gun Snap Up")  I18N("kn.controls.omProps", "Tool Gun Properties")
//   I18N("kn.controls.thirdPerson", "Third Person")
//   I18N("kn.controls.group.connection", "Connection")  I18N("kn.controls.group.sandbox", "Sandbox")  I18N("kn.controls.group.camera", "Camera")
static const char *const KN_ROWS[] = {"gameReload", "reconnect", "objectManager", "omUndo", "omRemove", "omRotate",
                                      "omPhysSnap", "omSnapDown", "omSnapUp", "omProps", "thirdPerson"};
static const char *const KN_GROUPS[] = {"Connection", "Sandbox", "Camera"};

static const char *row_label(const ControlRow *row) {
    for (size_t i = 0; i < sizeof(KN_ROWS) / sizeof(KN_ROWS[0]); i++) {
        if (strcmp(KN_ROWS[i], row->bind)) continue;
        char key[64];
        snprintf(key, sizeof(key), "kn.controls.%s", row->bind);
        return i18n_tr(key, row->label);
    }
    return row->label;
}

static const char *group_label(const char *group) {
    for (size_t i = 0; i < sizeof(KN_GROUPS) / sizeof(KN_GROUPS[0]); i++) {
        if (strcmp(KN_GROUPS[i], group)) continue;
        char key[64];
        size_t k = (size_t) snprintf(key, sizeof(key), "kn.controls.group.");
        for (const char *c = group; *c && k + 1 < sizeof(key); c++) key[k++] = (char) tolower((unsigned char) *c);
        key[k] = 0;
        return i18n_tr(key, group);
    }
    return group;
}
static const ControlTab TABS[] = {
    TAB("General", TAB_GENERAL), TAB("Chat & Toggles", TAB_CHAT), TAB("Interaction", TAB_INTERACTION),
    TAB("Spectate & Vote", TAB_SPECTATE), TAB("Modes", TAB_MODES),
};
#undef TAB
#define TAB_COUNT ((int) (sizeof(TABS) / sizeof(TABS[0])))

static struct {
    int loaded;
    int val[sizeof(V7_BINDS) / sizeof(V7_BINDS[0])];     // ControlBind.val
    int alt[sizeof(V7_BINDS) / sizeof(V7_BINDS[0])];     // ControlBind.altVal
    int binds[UI_CONTROL_COUNT];   // the KrunkNative id's primary key (val of its v7 bind, -1 none)
    int alts[UI_CONTROL_COUNT];    // its alternate key (altVal)
    char toggle_sets[TOGGLE_COUNT][SETTING_KEY_MAX]; // "" = -1 (None)
    char messages[MESSAGE_COUNT][MESSAGE_MAX];       // CONTROLS.chatMessages (cont_43_set ..)
    int scroll_bound;        // !scrollToSwap: a bind uses the wheel
    int changer;             // CONTROLS.inputChanger: V7_BINDS index waiting for "Press any Key", -1 none
    int changer_alt;
    int tab;                 // windows[6].tabIndex

    unsigned char keys[KEY_SLOTS];
    unsigned char did_pressed[KEY_SLOTS];
    int shoot;               // left mouse button (v7 routes it through binds.shoot: 10001)
    int aim;                 // right mouse button (binds.aim: 10003)
    int pending_swap;        // aba6: 1 swap(secondary) 2 melee 3 primary 4 equipment
    int pending_slot;        // KrunkNative Object Manager: Input.select_slot (0 none), kept until an input frame takes it
    int scroll_pressed;      // the wheel "key" is down until the end of this input frame

    int chat_focused;        // #chatInput has focus (focusChat(1) / focusChat())
    int swallowed_key;       // web: a capture consumed this key event before the page sees it
    int game_key;            // web: the game owned this key event (controls.js preventDefault)
    unsigned char logged[UI_CONTROL_COUNT];

    UiInputSettings settings; // Krunker defaults until client_apply_setting pushes values

    // Controller (CONTROLS.cntrlInput, inputType, a4/a5, speedLmt; client/src/gamepad.c)
    UiControllerInput cntrl;
    int input_cntrl;         // inputType == "cntrlInput" (the last look input was the right stick)
    float look_x, look_y;    // a4/a5 added by fakeMouse(2) since the last updateMouseInp
    float speed_lmt;         // speedLmt (gradSpeed); 1 = full speed
} g_ctl = {
    .changer = -1,
    .settings = {90.0f, 1, 1, 1, 1, 0, 0.8f, 1.0f, 1.0f, 1},
    // cntrlInput after the settings' set(): sensitivity*Cntrl x5, deadZoneL 0.3 (the
    // setting's val overrides cntrlInput's own 0.25), deadZoneR 0.25, triggerThres 0.1,
    // invertYCntrl off, vibration on, gradSpeed off, vibrationMlt 1.
    .cntrl = {5.0f, 5.0f, 5.0f, 5.0f, 0.3f, 0.25f, 0.1f, 0, 1, 0, 1.0f},
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
    else if (code < (int) (sizeof(KEYBOARD_MAP) / sizeof(KEYBOARD_MAP[0])) && KEYBOARD_MAP[code]) snprintf(buf, n, "%s", KEYBOARD_MAP[code]);
    else snprintf(buf, n, "undefined"); // keyboardMap[l] past the table
    return buf;
}

// ---------------------------------------------------------------- binding table

static int v7_index(const char *bind, const int index) {
    if (!bind) return -1;
    for (int i = 0; i < V7_BIND_COUNT; i++) {
        if (!strcmp(V7_BINDS[i].bind, bind) && V7_BINDS[i].index == (V7_BINDS[i].index < 0 ? -1 : index)) return i;
    }
    return -1;
}

static void ensure_loaded(void);

// CONTROLS.binds.<bind>[index].val (scalar binds: index -1), -1 when unknown / unbound.
int ui_controls_v7_val(const char *bind, const int index) {
    ensure_loaded();
    const int i = v7_index(bind, index);
    return i >= 0 ? g_ctl.val[i] : -1;
}

// v7 ControlBind name or altName -> (index, alt) (CONTROLS.getBindByName, scalar binds only)
static int v7_index_by_name(const char *name, int *alt) {
    char base[48];
    const size_t n = strlen(name);
    *alt = n > 4 && !strcmp(name + n - 4, "_alt");
    snprintf(base, sizeof(base), "%.*s", (int) (*alt ? n - 4 : n), name);
    for (int i = 0; i < V7_BIND_COUNT; i++) {
        if (V7_BINDS[i].index < 0 && !strcmp(V7_BINDS[i].name, base)) return i;
    }
    return -1;
}

// CONTROLS.checkScroll() (v10.0.2): scrollToSwap is off while any bind uses the wheel. v7 also
// counted the middle button; v10 binds it by default (Ping Position) and only checks the wheel.
static void check_for_scroll(void) {
    g_ctl.scroll_bound = 0;
    for (int i = 0; i < V7_BIND_COUNT; i++) {
        if (g_ctl.val[i] == KEY_SCROLL || g_ctl.alt[i] == KEY_SCROLL) g_ctl.scroll_bound = 1;
    }
}

// the KrunkNative ids follow their v7 binds (the v3.1.1 Streak 4/5 slots v7 dropped stay unbound)
static void sync_ids(void) {
    for (int id = 0; id < UI_CONTROL_COUNT; id++) g_ctl.binds[id] = g_ctl.alts[id] = KEY_UNBOUND;
    for (int i = 0; i < V7_BIND_COUNT; i++) {
        const int id = V7_BINDS[i].kn_id;
        if (id < 0) continue;
        g_ctl.binds[id] = g_ctl.val[i];
        g_ctl.alts[id] = g_ctl.alt[i];
    }
    check_for_scroll();
}

// parseInt(getSavedVal(key) || def)
static int saved_int(const char *key, const int def) {
    char buf[64];
    if (html_storage_get(key, buf, sizeof(buf)) < 0 || !buf[0]) return def;
    char *end;
    const long v = strtol(buf, &end, 10);
    return end == buf ? def : (int) v;
}

static void bind_storage_key(const int i, const int alt, char *buf, const size_t n) {
    snprintf(buf, n, "cont_%s%s", V7_BINDS[i].name, alt ? "_alt" : "");
}

// CONTROLS.toggleSets / chatMessages: getSavedVal("cont_<27..32|43..48>_set") || -1 / ""
static void load_sets(void) {
    char key[64];
    for (int i = 0; i < TOGGLE_COUNT; i++) {
        snprintf(key, sizeof(key), "cont_%d_set", TOGGLE_FIRST + i);
        char value[SETTING_KEY_MAX] = "";
        if (html_storage_get(key, value, sizeof(value)) < 0 || !strcmp(value, "-1")) value[0] = 0;
        snprintf(g_ctl.toggle_sets[i], sizeof(g_ctl.toggle_sets[i]), "%s", value);
    }
    for (int i = 0; i < MESSAGE_COUNT; i++) {
        snprintf(key, sizeof(key), "cont_%d_set", 43 + i);
        if (html_storage_get(key, g_ctl.messages[i], sizeof(g_ctl.messages[i])) < 0) g_ctl.messages[i][0] = 0;
    }
}

// new ControlBind(...) for every bind + toggleSets / chatMessages
void ui_controls_load(void) {
    char key[64];
    for (int i = 0; i < V7_BIND_COUNT; i++) {
        bind_storage_key(i, 0, key, sizeof(key));
        g_ctl.val[i] = saved_int(key, V7_BINDS[i].def);
        bind_storage_key(i, 1, key, sizeof(key));
        g_ctl.alt[i] = saved_int(key, V7_BINDS[i].alt_def);
    }
    load_sets();
    sync_ids();
    g_ctl.loaded = 1;
}

static void ensure_loaded(void) {
    if (!g_ctl.loaded) ui_controls_load();
}

int ui_controls_bind(const int id) {
    ensure_loaded();
    return id >= 0 && id < UI_CONTROL_COUNT ? g_ctl.binds[id] : KEY_UNBOUND;
}

int ui_controls_bind_alt(const int id) {
    ensure_loaded();
    return id >= 0 && id < UI_CONTROL_COUNT ? g_ctl.alts[id] : KEY_UNBOUND;
}

int ui_controls_v7_bind(const char *bind, const int index, const int alt) {
    ensure_loaded();
    const int i = v7_index(bind, index);
    return i < 0 ? KEY_UNBOUND : alt ? g_ctl.alt[i] : g_ctl.val[i];
}

// ControlBind.isKey(code): the key or its alternate
static int bind_is(const int id, const int code) {
    return code >= 0 && (g_ctl.binds[id] == code || g_ctl.alts[id] == code);
}

// ControlBind.update(code, alt): saveVal("cont_" + name, code)
static void v7_update(const int i, const int alt, const int code) {
    char key[64], value[16];
    if (alt) g_ctl.alt[i] = code;
    else g_ctl.val[i] = code;
    bind_storage_key(i, alt, key, sizeof(key));
    snprintf(value, sizeof(value), "%d", code);
    html_storage_set(key, value);
    sync_ids();
}

// resetSettings(): every cont_* key is removed, then CONTROLS.resetBinds().
void ui_controls_reset(void) {
    char key[64];
    for (int i = 0; i < V7_BIND_COUNT; i++) {
        for (int alt = 0; alt < 2; alt++) {
            bind_storage_key(i, alt, key, sizeof(key));
            html_storage_set(key, NULL);
        }
        g_ctl.val[i] = V7_BINDS[i].def;
        g_ctl.alt[i] = V7_BINDS[i].alt_def;
    }
    for (int i = 0; i < TOGGLE_COUNT; i++) {
        snprintf(key, sizeof(key), "cont_%d_set", TOGGLE_FIRST + i);
        html_storage_set(key, NULL);
    }
    for (int i = 0; i < MESSAGE_COUNT; i++) {
        snprintf(key, sizeof(key), "cont_%d_set", 43 + i);
        html_storage_set(key, NULL);
    }
    load_sets();
    g_ctl.changer = -1;
    sync_ids();
    g_ctl.loaded = 1;
    menu_rerender_if_open(7);
}

// ---------------------------------------------------------------- export / import

// CONTROLS.export(): scalar binds by name / altName when they differ from their default,
// arrays as "<bind>Keys" / "<bind>Keys_alt" with holes (JSON null), toggleSets and
// chatMessages when any is set. NULL when there is nothing (no "controls" key).
cJSON *ui_controls_export_json(void) {
    ensure_loaded();
    cJSON *out = cJSON_CreateObject();
    for (int i = 0; i < V7_BIND_COUNT; i++) {
        const V7Bind *b = &V7_BINDS[i];
        if (b->index < 0) {
            char alt_name[64];
            snprintf(alt_name, sizeof(alt_name), "%s_alt", b->name);
            if (g_ctl.val[i] != b->def) cJSON_AddNumberToObject(out, b->name, g_ctl.val[i]);
            if (g_ctl.alt[i] != b->alt_def) cJSON_AddNumberToObject(out, alt_name, g_ctl.alt[i]);
            continue;
        }
        if (b->index) continue; // each array once, from its element 0
        for (int alt = 0; alt < 2; alt++) {
            int last = -1, n = 0;
            for (int k = i; k < V7_BIND_COUNT && !strcmp(V7_BINDS[k].bind, b->bind); k++, n++) {
                if ((alt ? g_ctl.alt[k] != V7_BINDS[k].alt_def : g_ctl.val[k] != V7_BINDS[k].def)) last = n;
            }
            if (last < 0) continue;
            char name[64];
            snprintf(name, sizeof(name), "%sKeys%s", b->bind, alt ? "_alt" : "");
            cJSON *arr = cJSON_AddArrayToObject(out, name);
            for (int k = 0; k <= last; k++) {
                const int v = alt ? g_ctl.alt[i + k] : g_ctl.val[i + k];
                const int d = alt ? V7_BINDS[i + k].alt_def : V7_BINDS[i + k].def;
                cJSON_AddItemToArray(arr, v != d ? cJSON_CreateNumber(v) : cJSON_CreateNull());
            }
        }
    }
    int any = 0;
    for (int i = 0; i < TOGGLE_COUNT; i++) any |= g_ctl.toggle_sets[i][0] != 0;
    if (any) {
        cJSON *arr = cJSON_AddArrayToObject(out, "toggleSets");
        for (int i = 0; i < TOGGLE_COUNT; i++) cJSON_AddItemToArray(arr, g_ctl.toggle_sets[i][0] ? cJSON_CreateString(g_ctl.toggle_sets[i]) : cJSON_CreateNumber(-1));
    }
    any = 0;
    for (int i = 0; i < MESSAGE_COUNT; i++) any |= g_ctl.messages[i][0] != 0;
    if (any) {
        cJSON *arr = cJSON_AddArrayToObject(out, "chatMessages");
        for (int i = 0; i < MESSAGE_COUNT; i++) cJSON_AddItemToArray(arr, cJSON_CreateString(g_ctl.messages[i]));
    }
    if (!out->child) {
        cJSON_Delete(out);
        return NULL;
    }
    return out;
}

// filterCM: "<", ">", "'" and '"' dropped, "undefined" -> ""
static void filter_cm(const char *in, char *out, const size_t n) {
    size_t len = 0;
    for (; in && *in && len + 1 < n; in++) {
        if (*in == '<' || *in == '>' || *in == '\'' || *in == '"') continue;
        out[len++] = *in;
    }
    out[len] = 0;
    if (!strcmp(out, "undefined")) out[0] = 0;
}

static void store_message(const int i, const char *text) {
    char key[32];
    filter_cm(text, g_ctl.messages[i], sizeof(g_ctl.messages[i]));
    snprintf(key, sizeof(key), "cont_%d_set", 43 + i);
    html_storage_set(key, g_ctl.messages[i]);
}

static void store_toggle_set(const int i, const char *value) {
    char key[32];
    snprintf(g_ctl.toggle_sets[i], SETTING_KEY_MAX, "%s", value && strcmp(value, "-1") ? value : "");
    snprintf(key, sizeof(key), "cont_%d_set", TOGGLE_FIRST + i);
    html_storage_set(key, value && *value ? value : "-1");
}

// settings.import(): the "controls" object of v7.1.4 (array entries that are null keep the
// current bind; toggleSets / chatMessages replaced).
void ui_controls_import_json(const cJSON *controls) {
    ensure_loaded();
    if (!cJSON_IsObject(controls)) return;
    const cJSON *item;
    cJSON_ArrayForEach(item, controls) {
        const char *key = item->string;
        if (!key) continue;
        if (!strcmp(key, "toggleSets") || !strcmp(key, "chatMessages")) {
            if (!cJSON_IsArray(item)) continue;
            const int toggles = key[0] == 't';
            for (int i = 0; i < (toggles ? TOGGLE_COUNT : MESSAGE_COUNT); i++) {
                const cJSON *v = cJSON_GetArrayItem(item, i);
                const char *s = cJSON_IsString(v) ? cJSON_GetStringValue(v) : "";
                if (toggles) store_toggle_set(i, ui_setting_value(s) ? s : "");
                else store_message(i, s);
            }
            continue;
        }
        const char *keys = strstr(key, "Keys");
        if (keys && cJSON_IsArray(item)) { // moveKeys, streakKeys_alt, ...
            char bind[32];
            snprintf(bind, sizeof(bind), "%.*s", (int) (keys - key), key);
            const int alt = strstr(keys, "_alt") != NULL;
            int k = 0;
            const cJSON *v;
            cJSON_ArrayForEach(v, item) {
                const int i = v7_index(bind, k++);
                if (i >= 0 && cJSON_IsNumber(v)) v7_update(i, alt, (int) cJSON_GetNumberValue(v));
            }
            continue;
        }
        int alt;
        const int i = v7_index_by_name(key, &alt);
        if (i >= 0 && cJSON_IsNumber(item)) v7_update(i, alt, (int) cJSON_GetNumberValue(item));
    }
    g_ctl.changer = -1;
    sync_ids();
    menu_rerender_if_open(7);
}

// ---------------------------------------------------------------- the window

static void append_key_name(StrBuf *sb, const int code) {
    char name[32];
    sb_append_esc(sb, ui_controls_key_name(code, name, sizeof(name)));
}

static void append_index(StrBuf *sb, const int index) {
    if (index < 0) sb_append(sb, "undefined");
    else sb_appendf(sb, "%d", index);
}

// One key cell (the alternate key first, then the key): reset / unbind icons, the key name
// and the mouse button picture for 10001 / 10002 / 10003 / 20000.
static void append_key_cell(StrBuf *sb, const ControlRow *row, const int alt, const int code) {
    sb_append(sb, "<div style=\"float:right\"><span class=\"reset\" title=\"Reset Bind\" onclick=\"resetCont(&quot;");
    sb_append(sb, row->bind);
    sb_appendf(sb, "&quot;,%d,", alt);
    append_index(sb, row->index);
    sb_append(sb, ")\"><i class=\"material-icons\" style=\"font-size:40px;color:var(--yellow);\">refresh</i></span>"
                  "<span class=\"unbind\" title=\"Unbind\" onclick=\"unbindCont(&quot;");
    sb_append(sb, row->bind);
    sb_appendf(sb, "&quot;,%d,", alt);
    append_index(sb, row->index);
    sb_append(sb, ")\"><i class=\"material-icons\" style=\"font-size:40px;color:var(--red);\">delete_forever</i></span>"
                  "<span class=\"settText floatRNoC keyIcon\" id=\"cont");
    sb_append(sb, row->bind);
    if (row->index >= 0) sb_appendf(sb, "_%d", row->index);
    if (alt) sb_append(sb, "_alt");
    sb_append(sb, "\" onmouseover=\"playTick()\" onclick=\"changeCont(&quot;");
    sb_append(sb, row->bind);
    sb_appendf(sb, "&quot;,%d,", alt);
    append_index(sb, row->index);
    sb_append(sb, ")\">");
    append_key_name(sb, code);
    sb_append(sb, "</span>");
    if (code == KEY_MOUSE_BASE + 1 || code == KEY_MOUSE_BASE + 2 || code == KEY_MOUSE_BASE + 3 || code == KEY_SCROLL)
        sb_appendf(sb, "<div class=\"mouseBtn\" style=\"background-image: url(img/mouse/%d.png);\"></div>", code);
    sb_append(sb, "</div>");
}

// The <bind>Trigger select (_html.settings.input.select of scoreBoardTrigger / voiceChatTrigger).
static void append_trigger(StrBuf *sb, const char *bind) {
    char key[48];
    snprintf(key, sizeof(key), "%sTrigger", bind);
    if (strcmp(bind, "scoreBoard") && strcmp(bind, "voiceChat")) return;
    const cJSON *v = ui_setting_value(key);
    char cur[16] = "1"; // Continuous (Hold) by default
    if (v) ui_setting_to_string(v, cur, sizeof(cur));
    sb_appendf(sb, "<div style=\"float:right\"><select onchange=\"window.setSetting(&quot;%s&quot;, this.value)\" class=\"inputGrey2\">\n"
                   "\t\t\t\t\t\t\t<option value=\"0\"%s>Press (Toggle)</option>,<option value=\"1\"%s>Continuous (Hold)</option></select></div>",
               key, !strcmp(cur, "0") ? " selected=\"\"" : "", strcmp(cur, "0") ? " selected=\"\"" : "");
}

static void group_id(const char *group, char *out, const size_t n) {
    size_t k = 0;
    for (const char *p = group; *p && k + 1 < n; p++) {
        const char c = *p;
        out[k++] = (char) ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ? c
                         : c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : '_');
    }
    out[k] = 0;
}

// windows[6].gen(): the tab row, then each group (collapsible) with its rows, serialised the
// way the page holds it (double quotes, selected="").
char *ui_controls_window_html(void) {
    ensure_loaded();
    StrBuf sb = {0};
    const int tab = g_ctl.tab >= 0 && g_ctl.tab < TAB_COUNT ? g_ctl.tab : 0;
    sb_append(&sb, "<div id=\"bindsHeader\"><div class=\"bigMenTabs\" style=\"table-layout: fixed;margin-top: 18px!important;margin-bottom: unset;\">");
    for (int t = 0; t < TAB_COUNT; t++) {
        sb_appendf(&sb, "<div class=\"bigMenTab%s\" onclick=\"windows[6].switchTab(%d)\" style=\"padding-top:10px;padding-bottom:5px;\">",
                   t == tab ? " bigMenTabA" : "", t);
        sb_append_esc(&sb, TABS[t].name);
        sb_append(&sb, "</div>");
    }
    sb_append(&sb, "</div></div><div id=\"bindsHolder\">");
    const ControlTab *ct = &TABS[tab];
    for (int r = 0; r < ct->count; r++) {
        const ControlRow *row = &ct->rows[r];
        char gid[48];
        group_id(row->group, gid, sizeof(gid));
        if (r == 0 || strcmp(ct->rows[r - 1].group, row->group)) {
            if (r) sb_append(&sb, "</div>");
            sb_appendf(&sb, "<div class=\"setHed\" id=\"setBindHed_%s\" onclick=\"window.windows[6].collapseFolder(this)\">"
                            "<span class=\"material-icons plusOrMinus\">keyboard_arrow_down</span> ", gid);
            sb_append_esc(&sb, group_label(row->group));
            sb_appendf(&sb, "</div><div class=\"setBodH\" id=\"setBindBod_%s\">", gid);
        }
        sb_append(&sb, "<div class=\"settName\">");
        if (row->list == 1) {
            const char *set = g_ctl.toggle_sets[row->index];
            sb_appendf(&sb, "Toggle <select class=\"setToggle\" onchange=\"changeContSet(%d, this.value)\">", row->index);
            sb_appendf(&sb, "<option value=\"-1\"%s>None</option>", set[0] ? "" : " selected=\"\"");
            for (size_t t = 0; t < sizeof(TOGGLE_SETTINGS) / sizeof(TOGGLE_SETTINGS[0]); t++) {
                sb_append(&sb, "<option value=\"");
                sb_append_esc(&sb, TOGGLE_SETTINGS[t][0]);
                sb_appendf(&sb, "\"%s>", !strcmp(set, TOGGLE_SETTINGS[t][0]) ? " selected=\"\"" : "");
                sb_append_esc(&sb, TOGGLE_SETTINGS[t][1]);
                sb_append(&sb, "</option>");
            }
            sb_append(&sb, "</select>");
        } else if (row->list == 2) {
            sb_appendf(&sb, "Message <input type=\"text\" name=\"message%d\" class=\"inputGrey2\" placeholder=\"Message to Send\" style=\"float: none;\" value=\"", row->index);
            sb_append_esc(&sb, g_ctl.messages[row->index]);
            sb_appendf(&sb, "\" oninput=\"changeContMsg(%d, this.value)\">", row->index);
        } else {
            sb_append_esc(&sb, row_label(row));
        }
        append_trigger(&sb, row->bind);
        const int i = v7_index(row->bind, row->index);
        append_key_cell(&sb, row, 1, i >= 0 ? g_ctl.alt[i] : KEY_UNBOUND);
        sb_append(&sb, "<div class=\"bindSep\"></div>");
        append_key_cell(&sb, row, 0, i >= 0 ? g_ctl.val[i] : KEY_UNBOUND);
        sb_append(&sb, "</div>");
    }
    if (ct->count) sb_append(&sb, "</div>");
    sb_append(&sb, "</div>");
    return sb_finish(&sb);
}

static void set_cont_text(const int i, const int alt, const char *text) {
    char elem_id[64];
    if (i < 0) return;
    if (V7_BINDS[i].index >= 0) snprintf(elem_id, sizeof(elem_id), "cont%s_%d%s", V7_BINDS[i].bind, V7_BINDS[i].index, alt ? "_alt" : "");
    else snprintf(elem_id, sizeof(elem_id), "cont%s%s", V7_BINDS[i].bind, alt ? "_alt" : "");
    HtmlElem el = html_dom_by_id(elem_id);
    if (el) html_dom_set_html(el, text);
}

// windows[6].switchTab(i, force)
void ui_controls_switch_tab(const int tab, const int force) {
    if (tab < 0 || tab >= TAB_COUNT) return;
    if (g_ctl.tab != tab || force) {
        g_ctl.changer = -1;
        g_ctl.tab = tab;
        menu_rerender_if_open(7);
    }
}

// window.changeCont(bind, alt, index): re-render the window, then wait for a key.
void ui_controls_change(const char *bind, const int alt, const int index) {
    ensure_loaded();
    const int i = v7_index(bind, index);
    if (i < 0) return;
    menu_show_window(7, 1);
    g_ctl.changer = i;
    g_ctl.changer_alt = !!alt;
    set_cont_text(i, alt, I18N("settings.controls.press", "Press any Key"));
}

// window.unbindCont(bind, alt, index)
void ui_controls_unbind(const char *bind, const int alt, const int index) {
    ensure_loaded();
    const int i = v7_index(bind, index);
    menu_show_window(7, 1);
    g_ctl.changer = -1;
    if (i < 0) return;
    set_cont_text(i, alt, "UNBOUND");
    v7_update(i, !!alt, KEY_UNBOUND);
}

// window.resetCont(bind, alt, index): ControlBind.reset(alt)
void ui_controls_reset_bind(const char *bind, const int alt, const int index) {
    ensure_loaded();
    const int i = v7_index(bind, index);
    menu_show_window(7, 1);
    g_ctl.changer = -1;
    if (i < 0) return;
    v7_update(i, !!alt, alt ? V7_BINDS[i].alt_def : V7_BINDS[i].def);
    char name[32];
    set_cont_text(i, alt, ui_controls_key_name(g_ctl.val[i], name, sizeof(name))); // v7 writes the key's val in both cells
}

// window.changeContSet(index, setting): which setting a Toggle row flips.
void ui_controls_change_set(const int index, const char *setting) {
    ensure_loaded();
    if (index < 0 || index >= TOGGLE_COUNT) return;
    store_toggle_set(index, setting && *setting ? setting : "-1");
    menu_show_window(7, 1);
}

// window.changeContMsg(index, text): quick chat message (filterCM)
void ui_controls_change_msg(const int index, const char *text) {
    ensure_loaded();
    if (index < 0 || index >= MESSAGE_COUNT) return;
    store_message(index, text);
}

// KrunkNative deviation: Krunker keeps inputChanger after the controls window closes, so
// the next key pressed anywhere (even in game) is bound and window 7 reopens. Here the
// capture ends with the window.
int ui_controls_capturing(void) {
    if (g_ctl.changer < 0) return 0;
    const MenuUi *ui = menu_ui();
    if (ui->active_window != 7 || !menu_window_is_open()) {
        g_ctl.changer = -1;
        return 0;
    }
    return 1;
}

// CONTROLS N() with inputChanger set: updateBind + updateKeybindUI, re-render the window.
static void capture_bind(const int code) {
    const int i = g_ctl.changer;
    g_ctl.changer = -1;
    if (i < 0) return;
    v7_update(i, g_ctl.changer_alt, code);
    char name[32];
    printf("[controls] bind %s%s (%s) = %s\n", V7_BINDS[i].name, g_ctl.changer_alt ? "_alt" : "", V7_BINDS[i].bind,
           ui_controls_key_name(code, name, sizeof(name)));
    menu_show_window(7, 1);
#ifdef __EMSCRIPTEN__
    g_ctl.swallowed_key = 1; // the page's own keydown listener runs after GLFW's
#endif
}

int ui_controls_take_game_key(void) {
    const int g = g_ctl.game_key;
    g_ctl.game_key = 0;
    return g;
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
    if (g_ctl.chat_focused) return 1;
    const HtmlElem el = html_dom_active_element();
    if (!el) return 0;
    char tag[16] = "", editable[16] = "";
    html_dom_tag(el, tag, sizeof(tag));
    if (!strcmp(tag, "textarea") || !strcmp(tag, "select")) return 1;
    if (!strcmp(tag, "input")) {
        // Only fields that take typing. v7 checks tagName == "INPUT", but in the browser a click
        // into the game blurs the menu's switches and sliders; here a focused checkbox / range /
        // button (the Spectate switch, a volume slider) kept every game key, push to talk included,
        // away until something else took the focus.
        char type[24] = "";
        if (html_dom_get_attr(el, "type", type, sizeof(type)) < 0) return 1; // type defaults to text
        for (char *p = type; *p; p++) *p = (char) tolower((unsigned char) *p);
        static const char *const NON_TEXT[] = {"checkbox", "radio", "range", "button", "submit", "reset", "color", "file", "image", "hidden"};
        for (size_t i = 0; i < sizeof(NON_TEXT) / sizeof(NON_TEXT[0]); i++) {
            if (!strcmp(type, NON_TEXT[i])) return 0;
        }
        return 1;
    }
    return html_dom_get_attr(el, "contenteditable", editable, sizeof(editable)) >= 0 && strcmp(editable, "false");
}

// Typing in #chatInput or in the developer console (main.c: no hotkeys, chars to the page).
int ui_controls_chat_focused(void) {
    return g_ctl.chat_focused || ui_console_is_open();
}

static void chat_blur(void) {
    HtmlElem input = chat_input();
    if (input) html_dom_blur(input);
    ui_controls_chat_focus(0);
}

// The game took the input back (CONTROLS.enabled false -> true: menu_ui.c client_pointer_update).
// In the browser the click that locks the pointer again moves the focus off #chatInput or a menu
// field (v7 mousedown f2139 also blurs the chat itself); natively the lock also comes back with no
// click (Escape closing the menu, the next round, a spawn that closes the menu), and a field still
// focused then kept every key (ui_text_input_focused): the mouse turned the camera, nothing moved.
void ui_controls_release_focus(void) {
    if (g_ctl.chat_focused) chat_blur();
    const HtmlElem el = html_dom_active_element();
    if (el && ui_text_input_focused()) html_dom_blur(el);
}

// client_chat_send (client.h, CLIENT_HAVE_CHAT_SEND) lives in the net layer; a weak
// reference keeps this file linking while that side is absent from a build.
#if defined(CLIENT_HAVE_CHAT_SEND) && (defined(__GNUC__) || defined(__clang__))
extern void client_chat_send(Client *, const char *) __attribute__((weak));
#define UI_CHAT_SEND_AVAILABLE() (client_chat_send != NULL)
#elif defined(CLIENT_HAVE_CHAT_SEND)
#define UI_CHAT_SEND_AVAILABLE() 1
#endif

// Settings > Chat "Show Player Messages" off: plain text is not sent, but "/" lines still are (the
// server's commands answer the sender; /clear and the console's own commands are handled before).
int ui_controls_chat_text_allowed(const char *text) {
    return client_render_settings()->show_messages || (text && text[0] == '/');
}

// fp(msg): Krunker sends "ct" and shows the line when the server broadcasts it back.
static void chat_send(Client *client, const char *text) {
    if (!client) client = menu_ui()->client;
    if (ui_console_chat_command(client, text)) return; // the dev console mod's io.send wrapper: /bhop, singleplayer commands
    if (!ui_controls_chat_text_allowed(text)) return;  // showMessages off: a player line is not sent
#ifdef UI_CHAT_SEND_AVAILABLE
    if (client && UI_CHAT_SEND_AVAILABLE()) {
        client_chat_send(client, text); // net chat (client/src/net): the server's echo reaches the HUD
        return;
    }
#endif
    // No chat transport: echo the line locally.
    hud_ui_add_player_chat(client && client->menu_name[0] ? client->menu_name : "Guest", text, 1); // our own line: a human's
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

static int valid_id(const int id) {
    return id >= 0 && id < UI_CONTROL_COUNT;
}

// ControlBind.isPressed-style state of a bind: its key or its alternate is held.
int ui_controls_down(const int id) {
    ensure_loaded();
    return valid_id(id) && (code_down(g_ctl.binds[id]) || code_down(g_ctl.alts[id]));
}

// ControlBind.isKey(code) for a scalar v7 bind with no UI_CONTROL_* id (confirm, reset, ...)
static int v7_is(const char *bind, const int code) {
    const int i = v7_index(bind, -1);
    return i >= 0 && code >= 0 && (g_ctl.val[i] == code || g_ctl.alt[i] == code);
}

// The same for element `index` of an array bind (taunt[i], message[i]).
static int v7_is_at(const char *bind, const int index, const int code) {
    const int i = v7_index(bind, index);
    return i >= 0 && code >= 0 && (g_ctl.val[i] == code || g_ctl.alt[i] == code);
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
    sprays_wheel_close(); // keys are gone: the held spray wheel key too
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
    printf("[controls] %s: not available in Krunker Box Office (no-op)\n", what);
}

static void flip_setting(const char *key) {
    if (!key || !*key) return;
    const cJSON *v = ui_setting_value(key);
    const int on = cJSON_IsTrue(v) || (cJSON_IsNumber(v) && cJSON_GetNumberValue(v) != 0.0) ||
        (cJSON_IsString(v) && cJSON_GetStringValue(v)[0] && strcmp(cJSON_GetStringValue(v), "false") != 0);
    const HtmlArg value = {HTML_ARG_BOOL, on ? 0.0 : 1.0, NULL};
    ui_settings_set(key, &value);
}

static void show_player_list(void *user) {
    (void) user;
    menu_show_window(23, 0);
}

// window.pressButton(keyCode), the parts with an in-game effect.
static void press_button(Client *client, const int code) {
    // pListKey (Alt): only while the game owns the input (N()'s `!controls.enabled` return)
    // and not from a text field; CONTROLS.resetKeys(), CONTROLS.toggle(false) (the pointer
    // is freed, the menu shows), then setTimeout(() => showWindow(23), 0).
    if (bind_is(UI_CONTROL_PLAYER_LIST, code) && ui_controls_enabled(client) && !ui_text_input_focused()) {
        ui_controls_reset_keys();
        client_set_menu_open(client, 1);
        html_timer_add(0.0, show_player_list, NULL);
    }
    // sBoardKey: g6(1) -> hud_ui polls ui_controls_scoreboard_held()
    // window.Bf5: F free cam, H/0 objective cams, 1-9 players, R first/third person (raw keyCodes).
    // kpdVoteY / kpdVoteN (v10 :221340, spectating with a K.P.D case): the verdict steps (hud_votekick.c)
    if (client && spectate_active() && (v7_is("kpdVoteY", code) || v7_is("kpdVoteN", code)) &&
        hud_votekick_kpd_key(client, v7_is("kpdVoteY", code)))
        return;
    if (client && spectate_press_key(client, code)) return;
    // kickVoteY / kickVoteN (v10 :221373): a running vote kick takes [1] / [2] (the weapon keys
    // still swap: v10 handles both); hud_votekick.c checks the vote, the account and that we live
    if (client && v7_is("kickVoteY", code)) hud_votekick_key(client, 1);
    else if (client && v7_is("kickVoteN", code)) hud_votekick_key(client, 0);
    Player *me = client ? client->me : NULL;
    if (!me || !me->active) return;
    // sprayWheelKey (premium / sandbox): opens the wheel; else sprayKey: "spry" (v7) / "int" 0 (v3)
    social_spray_press(client, bind_is(UI_CONTROL_SPRAY, code), v7_is("sprayWheel", code));
    for (int i = 0; i < TAUNT_COUNT; i++) { // Prop team: "prpt", i
        if (v7_is_at("taunt", i, code)) social_taunt(client, i);
    }
    for (int i = 0; i < MESSAGE_COUNT; i++) { // f2028(i): quick chat
        if (v7_is_at("message", i, code)) social_quick_chat(client, g_ctl.messages[i]);
    }
    if (v7_is("confirm", code)) ui_interact_confirm(client); // confirmKey: "cnfm" (a pending KR trigger)
    if (bind_is(UI_CONTROL_INTERACT, code)) ui_interact_key(client, 0);     // io.send("int", type, id)
    if (bind_is(UI_CONTROL_INTERACT_SEC, code)) ui_interact_key(client, 1); // io.send("intSec", type, id)
    if (bind_is(UI_CONTROL_DROP, code)) ui_interact_drop(client);           // io.send("drpw")
    if (bind_is(UI_CONTROL_INSPECT, code)) player_inspect_weapon(me); // Game.players.wInspect(player)
    if (bind_is(UI_CONTROL_OM_UNDO, code)) object_manager_ui_key(client, OM_ACTION_UNDO);         // KrunkNative Object Manager
    if (bind_is(UI_CONTROL_OM_REMOVE, code)) object_manager_ui_key(client, OM_ACTION_REMOVE_AIM);
    const Weapon *held = me && me->active ? me->weapon : NULL; // the snap keys: only with a KN building gun in hand
    if (held && (held->kn_tool || held->kn_physgun) && (bind_is(UI_CONTROL_OM_SNAP_DOWN, code) || bind_is(UI_CONTROL_OM_SNAP_UP, code))) {
        om_build_step_snap(bind_is(UI_CONTROL_OM_SNAP_UP, code) ? 1 : -1);
        object_manager_ui_snap_changed();
    }
    if (held && held->kn_tool && bind_is(UI_CONTROL_OM_PROPS, code) && !om_build_props_key(client)) ui_notify(I18N("kn.om.nothingToEdit", "Nothing to edit at the crosshair"));
    for (int i = 0; i < 5; i++) { // v7.1.4 streak loadout: slots 0-2 (callInStreak); 3-4 are v3 leftovers
        if (bind_is(UI_CONTROL_STREAK0 + i, code)) {
            if (i < STREAK_SLOTS) combat_call_in_streak(client, i);
            else log_once(UI_CONTROL_STREAK0 + i, "killstreak (k)");
        }
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
    // v10.0.2 pressButton, `game.isSandbox` only (the offline sandbox: client->offline): Noclip,
    // Godmode, Smite, Unlimited Ammo and Kill on the local player, Spawn Bot / Clear Bots. The
    // sandbox's authority is the in-process server, so they go through its chat commands (each
    // answers with its own chat line, as v10's commands.god.on / "Cleared Bots").
    if (client && client->offline) {
        static const struct { const char *bind, *command; } SANDBOX_KEYS[] = {
            {"noclipSB", "noclip"}, {"godModeSB", "god"}, {"smiteSB", "smite"}, {"unlimitedSB", "unlimitedammo"},
            {"killSB", "kill"}, {"spawnBot", "bot_add 1"}, {"clearBots", "bot_kick all"},
        };
        for (size_t i = 0; i < sizeof(SANDBOX_KEYS) / sizeof(SANDBOX_KEYS[0]); i++) {
            if (v7_is(SANDBOX_KEYS[i].bind, code)) ui_console_exec(SANDBOX_KEYS[i].command);
        }
    }
}

// CONTROLS N(code, 1) while the game owns the input.
static void press_code(Client *client, const int code) {
    const int slot = key_slot(code);
    if (slot < 0 || g_ctl.keys[slot]) return;
    if (bind_is(UI_CONTROL_VOICE, code)) g_voice_key = 1;
    g_ctl.keys[slot] = 1;
    g_ctl.did_pressed[slot] = 1;
    // KrunkNative Object Manager: with extra loadout weapons the number row (1-9) picks slots and
    // keeps nothing else bound to those keys (streak call-ins, taunts) from acting on them
    if (code >= 49 && code <= 57 && client && client->me && player_loadout_base(client->me) < client->me->loadout_size) {
        g_ctl.pending_slot = code - 48;
        return;
    }
    if (bind_is(UI_CONTROL_SWAP, code)) g_ctl.pending_swap = 1;
    else if (bind_is(UI_CONTROL_MELEE, code)) g_ctl.pending_swap = 2;
    else if (bind_is(UI_CONTROL_PRIMARY, code)) g_ctl.pending_swap = 3;
    else if (bind_is(UI_CONTROL_EQUIP, code)) g_ctl.pending_swap = 4;
    press_button(client, code);
}

// CONTROLS N(code, 0) (+ releaseButton: the leaderboard is polled; the spray wheel's key sprays)
static void release_code(Client *client, const int code) {
    if (v7_is("sprayWheel", code)) social_spray_release(client ? client : menu_ui()->client, 1);
    const int slot = key_slot(code);
    if (bind_is(UI_CONTROL_VOICE, code)) g_voice_key = 0;
    if (slot >= 0) g_ctl.keys[slot] = 0;
}

// ---------------------------------------------------------------- GLFW events

// The Reload Game bind (F5): natively the client relaunches, as F4 used to (game_reload.h
// game_reload_relaunch_request: the same command line and window), and the new process comes back
// into the same game; the console's `reload` keeps the in-place reload. On the web it is the page
// reload (mods.ts F5: location.reload()); the address bar keeps ?game=<id> / ?offline=1&map= /
// ?sandbox (servers_ui.c), so the reload lands back in the same game.
static void reload_key(void) {
#ifdef __EMSCRIPTEN__
    g_ctl.game_key = 0; // no preventDefault: the browser's own F5 reloads too
    app_reload(NULL);
#else
    printf("[controls] reload key: relaunch\n");
    game_reload_relaunch_request("F5");
#endif
}

// The Reconnect bind (unbound by default): the console's `reconnect` natively (the same server
// or lobby, the offline game restarted on its map); the page reload on the web.
static void reconnect_key(void) {
#ifdef __EMSCRIPTEN__
    g_ctl.game_key = 0; // no preventDefault: the browser's own F5 reloads too
    app_reload(NULL);
#else
    printf("[controls] reconnect key\n");
    ui_console_exec("reconnect");
#endif
}

int ui_controls_key_event(Client *client, const int key, const int scancode, const int action, const int mods) {
    ensure_loaded();
    const int code = ui_controls_dom_key(key);
    const int press = action == GLFW_PRESS;
    const int menu = html_ui_is_menu_open();
    g_ctl.swallowed_key = 0; // only ever about the key event being dispatched right now
    // v10 keydown / keyup (Q / U with no input type): gamepad.active = false, inputType = undefined
    if (action != GLFW_REPEAT) {
        gamepad_set_active(0);
        g_ctl.input_cntrl = 0;
    }
    // controls.js keydown/keyup: `if (controls.enabled) e.preventDefault()`, decided before
    // the key acts (pListKey frees the pointer); the chat key's keydown goes to
    // pressButton without it. The web backend reads it (web_dom.c kn_ui_key_prevent).
    g_ctl.game_key = ui_controls_enabled(client) && !ui_text_input_focused() && !(press && code >= 0 && bind_is(UI_CONTROL_CHAT, code));

    if (press && ui_controls_capturing()) {
        // Krunker's keydown hands the chat key to pressButton (it focuses the chat)
        // instead of N(), so it never reaches the capture.
        if (code >= 0 && !bind_is(UI_CONTROL_CHAT, code)) capture_bind(code);
        return 1;
    }

    // Reload Game: in game and in the menu, also with the console open; while typing only on a
    // function key (F5 never types; the browser's F5 reloads from a text field too)
    if (press && code >= 0 && bind_is(UI_CONTROL_GAME_RELOAD, code) && ((code >= 112 && code <= 123) || (!g_ctl.chat_focused && !ui_text_input_focused()))) {
        reload_key();
        return 1;
    }
    if (ui_console_key_event(client, key, scancode, action, mods)) return 1; // dev console + mods.ts keys (ui_console.c)

    // Reconnect works in game and in the menu, but not while typing (chat, a menu field)
    if (press && code >= 0 && bind_is(UI_CONTROL_RECONNECT, code) && !g_ctl.chat_focused && !ui_text_input_focused()) {
        reconnect_key();
        return 1;
    }
    // Object Manager (F1): in game and in the menu, not while typing (its own search box excepted)
    if (press && code >= 0 && bind_is(UI_CONTROL_OBJECT_MANAGER, code) && !g_ctl.chat_focused &&
        (!ui_text_input_focused() || object_manager_ui_search_focused())) {
        g_ctl.game_key = 1; // web: preventDefault, the browser's own F1 (help) must not open
        object_manager_ui_toggle(client);
        return 1;
    }

    // Third person (KrunkNative): in game, not while typing
    if (press && code >= 0 && !menu && bind_is(UI_CONTROL_THIRD_PERSON, code) && !g_ctl.chat_focused && !ui_text_input_focused()) {
        third_person_toggle(client);
        return 1;
    }

    if (g_ctl.chat_focused) {
        if (press && code >= 0 && bind_is(UI_CONTROL_CHAT, code)) return chat_key(client);
        if (menu) return 0; // the page edits the focused input
        if (action == GLFW_RELEASE) release_code(client, code); // keyups still reach N()
        if (press && key == GLFW_KEY_ESCAPE) {
            chat_blur();
            return 0; // main.c opens the menu (Krunker: the pointer lock is lost)
        }
        html_ui_key(key, scancode, action, mods); // typing goes to #chatInput, not the player
        return 1;
    }

    // pressButton(27): N() lets Escape through while the controls are disabled, and
    // pressButton toggles the end-screen results <-> the menu (UI.toggleEndScreen).
    // A menu window open over the results' menu closes first (the page's Escape, ui_handlers.c),
    // like everywhere else in the menu; the next Escape brings the results back.
    if (press && code == 27 && !(menu && menu_window_is_open()) && hud_ui_toggle_end_screen(client)) {
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
    else release_code(client, code);
    return 0;
}

int ui_controls_mouse_event(Client *client, const int button, const int action, const int in_game) {
    ensure_loaded();
    const int code = mouse_code(button);
    if (!in_game) {
        // controls.js mousedown: `which >= 0 && inputChanger != null` -> every button binds
        // while "Press any Key" waits (v7: LMB/RMB are rebindable too)
        if (action == GLFW_PRESS && code > 0 && ui_controls_capturing()) {
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
        // v7.1.4: all mouse buttons go through the binds as 10000 + which (shoot = 10001
        // LMB, aim = 10003 RMB by default), so they can be rebound
        press_code(client, code);
    } else if (action == GLFW_RELEASE) {
        gamepad_set_active(0); // canvas mouseup: aa(event) with inputType != "cntrlInput"
        release_code(client, code);
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
    // v7 controls.js wheel: an open spray wheel takes the scroll (sprayWheelChange), no key 20000
    if (social_spray_scroll(dy)) return 1;
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
    else release_code(client, code);
}

// Q(null, binds[id].val) (down) / U(null, binds[id].val) (up)
void ui_controls_controller_key(Client *client, const int id, const int down) {
    ensure_loaded();
    if (!valid_id(id)) return;
    g_ctl.input_cntrl = 1; // Q / U(..., "cntrlInput"): inputType = "cntrlInput"
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

// CONTROLS.fakeMouse(type, x, y, 0, "cntrlInput") (v10): 0 = the canvas mousedown with which 0,
// 1 = mouseup, 2 = mousemove by x/y. v10's controller mapping has no MOUSE entry, so the trigger
// shoots through its "shoot" KEY (gamepad.c); these only take the pointer back into the game and
// press / release the unbound mouse code 10000 + which (v3.1.1's which-0 branch shot: UCUz1).
void ui_controls_fake_mouse(Client *client, const int type, const float dx, const float dy) {
    if (type == 0) {
        g_ctl.input_cntrl = 1;              // inputType = "cntrlInput" (gamepad.active stays)
        if (!client || !client->game.ready) return; // masterLock
        if (!ui_controls_enabled(client)) {
            ui_controls_toggle(client, 1);
            return;
        }
        if (g_ctl.chat_focused) chat_blur();
        press_code(client, 10000);          // Q(null, 10000 + which, "cntrlInput"); which 0: no spectMode
    } else if (type == 1) {
        g_ctl.input_cntrl = 1;
        if (!hud_ui_end_screen_interactive()) release_code(client, 10000); // !locked: U(null, 10000 + which)
    } else if (type == 2) {
        if (!ui_controls_enabled(client)) return;
        // window.noMouseInp: the first-person spectator view takes no look input. The
        // other spectator cams turn with these sums (spectate_tick -> ui_controls_fill_input).
        if (spectate_active() && ((spectate_first_person() && spectate_target_uid() >= 0) || spectate_objective_cam())) return;
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
void ui_controls_fill_look(Client *client, Input *input, const float dx, const float dy) {
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
    const int aiming = g_ctl.aim || ui_controls_down(UI_CONTROL_AIM);
    const float sens_x = g_ctl.input_cntrl ? (aiming ? c->aim_x : c->sens_x) : (aiming ? s->aim_sensitivity_x : s->sensitivity_x);
    const float sens_y = g_ctl.input_cntrl ? (aiming ? c->aim_y : c->sens_y) : (aiming ? s->aim_sensitivity_y : s->sensitivity_y);
    const int invert = g_ctl.input_cntrl ? c->invert : s->invert_y;
    input->x_dir -= look_y * base * sens_y * (invert ? -1.0f : 1.0f);
    input->y_dir -= look_x * base * sens_x;
    float touch_x, touch_y; // fakeMouse(2, dx, dy, 0, "mblInput"): mobile sensitivities (ui_mobile.c)
    if (ui_mobile_take_look(aiming, &touch_x, &touch_y)) {
        input->x_dir -= touch_y * base;
        input->y_dir -= touch_x * base;
    }
}

void ui_controls_fill_input(Client *client, Input *input, const float dx, const float dy) {
    ui_controls_fill_look(client, input, dx, dy);

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

    // v7.1.4 sends jump while held (index.js gameLoop: CONTROLS.isPressed("jump")); tryJump still wants
    // a release between jumps unless autoJump (v3.1.1: the press frame only)
    input->jump = ui_controls_down(UI_CONTROL_JUMP);
    input->crouch = ui_controls_down(UI_CONTROL_CROUCH);
    input->reload = ui_controls_down(UI_CONTROL_RELOAD);
    input->shoot = g_ctl.shoot || ui_controls_down(UI_CONTROL_SHOOT);
    input->scope = g_ctl.aim || ui_controls_down(UI_CONTROL_AIM);
    input->swap = input_swap(client ? client->me : NULL);
    input->select_slot = (unsigned char) g_ctl.pending_slot; // Object Manager slot pick (number keys)
    g_ctl.pending_slot = 0;
    input->slowdown = 1.0f - roundf(g_ctl.speed_lmt * 10.0f) / 10.0f; // b9[12]: 1 - speedLmt.round(1)
    // WP-C v7.1.4 input[13] isPressed("resetLast") ? 2 : isPressed("reset") ? 1 : 0, input[14] isPressed("interact")
    input->reset = ui_controls_down(UI_CONTROL_RESET_LAST) ? 2 : ui_controls_down(UI_CONTROL_RESET) ? 1 : 0;
    input->interact_held = ui_controls_down(UI_CONTROL_INTERACT) ? 1 : 0;
    ui_bhop_fill_input(client, input); // v3.1.1 mod patchInputs: bhop jump/crouch slots (ui_bhop.c)
}

// KrunkNative Object Manager: equip loadout slot `slot` with the next input frame (-1 cancels).
void ui_controls_select_slot(const int slot) {
    g_ctl.pending_slot = slot >= 0 && slot < 126 ? slot + 1 : 0;
}

// The pending slot pick for an input frame the controls do not fill (a window open over the
// game: the Object Manager's gun tiles switch at once), 0 none
int ui_controls_take_slot(void) {
    const int slot = g_ctl.pending_slot;
    g_ctl.pending_slot = 0;
    return slot;
}

// After the input frame: didPressed, aba6 and the wheel "key" are one-frame events.
void ui_controls_end_frame(void) {
    memset(g_ctl.did_pressed, 0, sizeof(g_ctl.did_pressed));
    g_ctl.pending_swap = 0;
    if (g_ctl.scroll_pressed) {
        release_code(NULL, KEY_SCROLL);
        g_ctl.scroll_pressed = 0;
    }
}

// ---------------------------------------------------------------- inline handlers

static const char *h_str(const HtmlArg *args, const int argc, const int i) {
    return i < argc && args[i].kind == HTML_ARG_STR && args[i].str ? args[i].str : "";
}

// 0/1 (alt), an array index or `undefined` (-1)
static int h_int(const HtmlArg *args, const int argc, const int i, const int fallback) {
    if (i >= argc) return fallback;
    if (args[i].kind == HTML_ARG_NUM || args[i].kind == HTML_ARG_BOOL) return (int) args[i].num;
    if (args[i].kind == HTML_ARG_STR && args[i].str) {
        char *end;
        const long v = strtol(args[i].str, &end, 10);
        return end != args[i].str ? (int) v : fallback;
    }
    return fallback;
}

static const char *h_value(const HtmlEvent *ev, const HtmlArg *args, const int argc, const int i, char *buf, const size_t n) {
    if (i < argc && args[i].kind == HTML_ARG_THIS_VALUE) {
        if (ev && ev->value && ev->value[0]) return ev->value;
        if (ev && ev->target && html_dom_get_value(ev->target, buf, n) >= 0) return buf; // <select> snapshot reads ""
        return "";
    }
    return h_str(args, argc, i);
}

// windows[6].collapseFolder(hed): setBindHed_<g> / setBindBod_<g>
static void collapse_folder(const HtmlElem hed) {
    char id[96], body_id[112], display[16];
    if (!hed || html_dom_get_attr(hed, "id", id, sizeof(id)) < 0 || strncmp(id, "setBindHed_", 11)) return;
    snprintf(body_id, sizeof(body_id), "setBindBod_%s", id + 11);
    const HtmlElem body = html_dom_by_id(body_id);
    if (!body) return;
    const int closed = html_dom_get_style(body, "display", display, sizeof(display)) >= 0 && !strcmp(display, "none");
    html_dom_set_display(body, closed ? "" : "none");
    const HtmlElem icon = html_dom_query(hed, ".plusOrMinus");
    if (icon) html_dom_set_text(icon, closed ? "keyboard_arrow_down" : "keyboard_arrow_right");
}

// Inline handlers this module owns (ui_handlers.c h_fallback); -1 = not ours.
int ui_controls_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    if (!name) return -1;
    char buf[MESSAGE_MAX + 16];
    if (!strcmp(name, "changeCont")) ui_controls_change(h_str(args, argc, 0), h_int(args, argc, 1, 0), h_int(args, argc, 2, -1));
    else if (!strcmp(name, "unbindCont")) ui_controls_unbind(h_str(args, argc, 0), h_int(args, argc, 1, 0), h_int(args, argc, 2, -1));
    else if (!strcmp(name, "resetCont")) ui_controls_reset_bind(h_str(args, argc, 0), h_int(args, argc, 1, 0), h_int(args, argc, 2, -1));
    else if (!strcmp(name, "changeContSet")) ui_controls_change_set(h_int(args, argc, 0, -1), h_value(ev, args, argc, 1, buf, sizeof(buf)));
    else if (!strcmp(name, "changeContMsg")) ui_controls_change_msg(h_int(args, argc, 0, -1), h_value(ev, args, argc, 1, buf, sizeof(buf)));
    else if (!strcmp(name, "windows.6.switchTab")) ui_controls_switch_tab(h_int(args, argc, 0, 0), h_int(args, argc, 1, 0));
    else if (!strcmp(name, "windows.6.collapseFolder")) collapse_folder(argc > 0 && args[0].kind == HTML_ARG_THIS && args[0].num > 0 ? (HtmlElem) args[0].num : ev ? ev->target : 0);
    else return -1;
    return 1;
}
