#pragma once
// C UI logic layer over the DOM-ops API (html_dom.h): the C port of the former menu
// and HUD JavaScript (krunknative.js, hud.js) that drives Krunker's v3.1.1 page without
// JavaScript. Contract: doc/html-ui-engine.md; plan: doc/html-engine-plan.md, "C UI
// logic". Built into every client: with KRUNKNATIVE_HTML_ENGINE=OFF the DOM ops are the
// no-ops of client/src/html/stub.c and html_ui_available() is 0.
//
//   client/src/ui/menu_ui.c      MenuUi state machine + the net/connection glue
//                                (client_menu_bind/tick, client_set_menu_open)
//   client/src/ui/ui_handlers.c  the inline-handler table (every on* name in the markup)
//   client/src/ui/ui_windows.c   window markup loading (ui/windows) + templating
//   client/src/ui/ui_settings.c  settings store (knSettings) + client_apply_setting
//   client/src/ui/hud_ui.c       HUD over DOM ops (reads MenuUi.settings, e.g. show_fps)
//
// Everything here runs on the main thread from html_ui_update()/client_menu_tick().
#include <client.h>
#include <html_dom.h>

// Window ids: Krunker's numeric ids, plus the two non-numbered windows krunknative.js
// addressed as 'host' and 'join'.
#define MENU_WINDOW_NONE 0
#define MENU_WINDOW_HOST (-1)
#define MENU_WINDOW_JOIN (-2)

#define MENU_CLASS_MAX 16
#define MENU_MAP_MAX 64
#define MENU_NAME_MAX 32

// Region label Krunker shows in #menuRegionLabel once it knows the game's region;
// "..." until then. KrunkNative has one server.
#define MENU_REGION_NAME "Local"

typedef struct {
    int index;
    const char *name;    // borrowed from Game.classes / g_classes
    const char *weapon;  // primary weapon name, "" when unknown
    int hp;
    char icon[48];       // textures/classes/icon_N.png
} MenuClass;

typedef struct {
    int index;
    char name[48];
} MenuMap;

// Settings the engine acts on (CORE_SETTINGS in krunknative.js), as last applied
// through client_apply_setting(). Other systems (HUD, input) read them from here.
typedef struct {
    float sensitivity_x, sensitivity_y;
    float aim_sensitivity_x, aim_sensitivity_y;
    int invert_y;
    float fov;            // degrees (Krunker setting); applied to Camera.fov in radians
    float ads_fov_mlt;
    float scale_ui;       // 0.8 default -> uiCustomScale = 2 - scale_ui
    int ui_scaling;       // 0: #uiBase is not scaled at all
    int resolution_pct;   // `resolution` (percent)
    int shadows, soft_shadows, high_res_shadows, ambient_shading;
    int show_fps;
    float speed_scale, dmg_scale;
    float ambient_volume, guns_volume, player_volume, ui_volume;
} ClientSettings;

typedef struct {
    Client *client;
    int dom_ready;          // krunknative.js `bridgeUp`: the page exists and setup ran
    int menu_open;          // what the DOM currently shows (applyMenuOpen)

    // mirror of the old `state` event
    int in_game, connected, ready;
    char map_name[64];
    char mode_name[32];
    int name_set;           // a nickname was saved/committed (else #accName stays empty)

    char connect_error[160]; // text of the connection error screen; "" while connected/connecting
    int connect_timer;       // CONNECT_TIMEOUT_MS watchdog (html_timer id, 0 = none)
    int bubble_timer;        // speech bubble hide timer
    int rect_timer;          // coalesced sendPreviewRect (setTimeout 0)
    int invite_timer;        // copyInviteLink "Copied" -> "Invite"

    int active_window;       // MENU_WINDOW_* / Krunker id currently in #menuWindow
    int settings_tab;
    int selected_map;        // Game.maps index picked in the host/games windows, -1 = none

    MenuClass classes[MENU_CLASS_MAX];
    int class_count;
    MenuMap maps[MENU_MAP_MAX];
    int map_count;

    char last_rect[96];      // last previewRect handed to class_preview.c (dedupe key)
    char last_instructions[1024];
    float viewport_w, viewport_h; // last scaleBase() viewport (resize fallback)

    ClientSettings settings;

    // state diffing (was menu_last / menu_emit_state)
    struct {
        int valid, in_game, connected, ready, map_index, mode_id, class_index, health;
    } last;
    const void *last_classes;
    size_t last_class_count;
    const void *last_maps;
    size_t last_map_count;
} MenuUi;

MenuUi *menu_ui(void);

// ---- menu_ui.c: krunknative.js functions, one for one
void menu_apply_open(int open);                  // applyMenuOpen
void menu_request_open(int open);                // requestMenu -> client_set_menu_open
void menu_update_instructions(void);             // updatePlayInstructions (4 states)
void menu_update_top_bar(void);                  // updateTopBar
void menu_update_class_box(void);                // updateClassBox
void menu_show_speech_bubble(const char *text, double ms); // showSpeechBubble (2,100 ms default)
void menu_show_window(int id, int force);        // showWindow
void menu_toggle_window(int open);               // UI.toggleWindow
void menu_close_window(void);                    // closeWindow
void menu_rerender_if_open(int id);              // rerenderIfOpen
int  menu_window_is_open(void);                  // #windowHolder display == block
void menu_scale_base(void);                      // scaleBase
void menu_send_preview_rect(void);               // sendPreviewRect -> client_class_preview_set_rect
void menu_schedule_rect(void);                   // scheduleRect
void menu_play(void);                            // play
void menu_host_offline(void);                    // hostOffline
void menu_retry_connect(void);                   // retryConnect
void menu_set_class_index(int index, int silent);// setClassIndex
void menu_select_class(int index);               // selectClass
void menu_select_map(int index);                 // knSelectMap
void menu_commit_account_name(void);             // commitAccountName
void menu_set_active_tab(int n);                 // setActiveTab (windows[3]/[9].switchTab)
void menu_log(const char *fmt, ...);             // KN.log('info', ...)
const MenuClass *menu_class_at(int index);       // classAt
const MenuMap *menu_map_by_name(const char *name); // mapByName

// ---- hud_ui.c: game-event entry points (HUD over DOM ops)
//   hud_ui_add_killfeed: one kill-feed row; killer/victim are player names, weapon_icon is
//     the Weapon.icon stem ("icon_2") or NULL/"" for a text-only row.
//   hud_ui_hit: restarts the hit-marker animation (kill = red, 1.2x bars).
void hud_ui_add_killfeed(Client*, const char *killer, const char *victim, const char *weapon_icon, int is_headshot);
void hud_ui_hit(Client*, int kill);

// ---- ui_handlers.c
void ui_handlers_install(MenuUi *ui);            // html_ui_set_handlers + the non-inline listeners

// ---- ui_windows.c
// Small string builder shared by the window generators and the menu.
typedef struct {
    char *data;
    size_t len, cap;
} StrBuf;
void  sb_append(StrBuf*, const char*);
void  sb_append_n(StrBuf*, const char*, size_t);
void  sb_append_esc(StrBuf*, const char*);        // esc(): & < > " '
void  sb_appendf(StrBuf*, const char *fmt, ...);
char *sb_finish(StrBuf*);                          // returns the (always non-NULL) buffer; StrBuf is reset
void  sb_free(StrBuf*);

int         ui_windows_load(void);                       // ui/windows/windows.json + files; 1 on success
const char *ui_windows_html(const char *key);            // windows[key].file content, "" when absent
const char *ui_windows_part(const char *key, const char *part); // "card" / "item" / "mapCard"
int         ui_settings_tab_count(void);
const char *ui_settings_tab_html(int tab);
const char *ui_settings_head_html(void);
// WINDOWS[id].gen(): malloc'd markup for #menuWindow, header text and stickyHeader
// flag. NULL when the id is not available in KrunkNative.
char *ui_window_generate(int id, const char **header, int *sticky);
void  ui_window_post(int id);                             // WINDOWS[id].post()
const char *ui_window_key(int id);                        // "1".."33", "host", "join"; NULL unknown
const char *ui_map_thumb(const char *name);               // mapThumb()
int         ui_host_checked_mode(void);                   // createPrivateRoom: first checked hostable #gameMode<id>, -1 none
const char *ui_host_checked_map(void);                    // createPrivateRoom: first checked #gameMap<i>, NULL none
int ui_host_checked_map_index(void);                       // the same as a map catalogue index (map_catalog.h), -1 none

// ---- ui_settings.c
void ui_settings_load(void);                     // loadSettings (knSettings)
void ui_settings_collect_defaults(void);         // collectDefaults (scraped from the settings tabs)
const cJSON *ui_setting_value(const char *key);  // settingValue: saved else default, NULL unknown
const cJSON *ui_setting_saved(const char *key);  // settings[key] only
int  ui_setting_number(const char *key, double *out); // 1 when settingValue(key) is a number
int  ui_setting_is_false(const char *key);       // settingValue(key) === false
void ui_setting_to_string(const cJSON *value, char *buf, size_t n); // String(value)
void ui_settings_set(const char *key, const HtmlArg *value); // setSetting(key, value)
void ui_settings_set_string(const char *key, const char *value);
void ui_settings_reset(void);                    // resetSettings
void ui_settings_export(void);                   // exportSettings (log)
char *ui_settings_export_text(void);             // exportSettings(true): the text, malloc'd (account settings upload)
void ui_settings_apply_values(HtmlElem root);    // applySettingValues
void ui_settings_push_saved(void);               // pushSavedSettings (CORE_SETTINGS first)
void ui_settings_search(const char *query);      // windows[0].searchList body
void ui_settings_collapse_folder(HtmlElem header); // windows[0].collapseFolder
// Apply one setting to the game (value as String(value): numbers, "true"/"false", text).
void client_apply_setting(Client*, const char *key, const char *value);

// ============================================================================ scenario
// BEGIN ui_scenario block: `client_bin --render-scenario` (tools/uidiff) and the game-event
// entry points it drives. The HUD/menu functions below are real UI logic (usable by game
// events); only ui_scenario_* and the hud_ui_set_* overrides are scenario-mode tools.

// ---- hud_ui.c: more game-event entry points
typedef struct {
    const char *killer;  // killer alias; NULL/"" = "You died" (no card)
    const char *weapon;  // weapon (or skin) name shown as [name]
    int level;           // killer level ("lvl N", at least 1)
    int score;           // +score for the kill (0 = no score line)
    int class_index;     // killer class -> textures/classes/icon_N.png
} HudKillCard;
// Krunker g1() for the local player: #killCard markup + UI.toggleGameUI(false).
void hud_ui_show_death(Client*, const HudKillCard *card);
void hud_ui_hide_death(Client*);                 // respawn: UI.toggleGameUI(true)
// UI.showEndScreen(false) + fq()/fD(): result 1 victory, 0 defeat, -1 draw; next_round "MM:SS".
void hud_ui_show_end_screen(Client*, int result, const char *next_round);
// Next round started (net INIT): undo hud_ui_show_end_screen (#endUI hidden, menu or HUD
// back per the current state, damage indicators cleared).
#define HUD_UI_HAVE_HIDE_END_SCREEN 1
void hud_ui_hide_end_screen(Client*);
// GUI.endScreen.changeTab(id): "leaderboard", "performance", "clanwar" or "medals".
void hud_ui_end_screen_tab(const char *tab);
int  hud_ui_end_screen_up(void);            // END to INIT (title animation or results): no menu over it unless toggled
int  hud_ui_end_screen_interactive(void);   // end-screen results up: pointer free, mouse goes to the page
int  hud_ui_toggle_end_screen(Client*);     // UI.toggleEndScreen: results <-> menu (Escape, controller Start); 1 = acted
// "<victim> committed <random config.suicides entry>" kill-feed row (no killer/weapon).
#define HUD_UI_HAVE_KILLFEED_SUICIDE 1
void hud_ui_add_killfeed_suicide(Client*, const char *victim, int victim_is_you);
// Damage-direction indicator (Krunker "h" packet -> aV.hits, game-overlay am()): the
// local player was hit by a dealer at world x/z; fades over config.hitLife (2 s).
#define HUD_UI_HAVE_DAMAGE_FROM 1
void hud_ui_damage_from(Client*, float x, float z);
// Damage number (game-overlay NJ2 / dmgNums) at the world hit point: `value` damage dealt,
// `crit` = headshot (critColor). Drifts up for 1 s; showDMG/dmgColor/critColor/dmgScale apply.
#define HUD_UI_HAVE_DAMAGE_NUMBERS 1
void hud_ui_damage_number(Client*, float x, float y, float z, int value, int crit);
// client_hud_html_tick reads the live match values itself: timer = Client.net_round_seconds
// (>= 0, else the configured match length), score = Player.score, ping = Client.net_ping_ms,
// scoreboard = Game.players sorted by score (Player.name). Scenario overrides still win.
#define HUD_UI_HAVE_LIVE_VALUES 1
// One #chatList row (Krunker gB/gy): name+msg = player line, else color = server line
// (type 2), else a plain game line (type 1).
void hud_ui_add_chat(const char *name, const char *msg, const char *color);
void hud_ui_clear_chat(void);                    // empty #chatList, no join lines seeded
// hud_ui_add_killfeed with explicit "You" flags and the row's age (into its 4 s life). A NULL
// killer (not "You") is a self-kill: weapon + victim only. Old Scoreboard puts the row in #chatList.
void hud_ui_add_killfeed_ex(Client*, const char *killer, int killer_is_you, const char *victim, int victim_is_you,
                            const char *weapon_icon, int is_headshot, double age_ms);
void hud_ui_hit_elapsed(Client*, int kill, double elapsed_ms); // hud_ui_hit `elapsed_ms` ago

// Scenario overrides (NULL clears): the `state.hud` payload replaces the values the HUD
// samples from the game; `state.scoreboard` rows replace the local-player-only board.
void hud_ui_set_override(const cJSON *hud);
void hud_ui_set_override_scoreboard(const cJSON *rows);
void hud_ui_set_clock(double now_s);             // HUD clock (hit marker, 20 Hz pacing); < 0 = GLFW clock

// ---- menu_ui.c
void menu_set_region_label(const char *name);    // #menuRegionLabel text when connected; NULL = MENU_REGION_NAME
void menu_set_game_name(const char *name);       // #mapInfo (mode alias + "_" + map) + #infoHolder; NULL hides

// ---- ui_scenario.c (native engine builds only)
#if !defined(__EMSCRIPTEN__)
#define UI_SCENARIO_SUPPORTED 1
int  ui_scenario_begin(const char *scenario_path, const char *out_png); // parse + isolate storage; 0 on error
int  ui_scenario_active(void);
int  ui_scenario_client_chrome(void);            // state.clientChrome: Exit item + Client tab (ui_client_settings.c)
void ui_scenario_window_size(int *w, int *h);    // the scenario `size`
const char *ui_scenario_map_path(void);          // map JSON for state.mapName, NULL = default rotation
int  ui_scenario_run(Client*);                   // frames + state + PNG; process exit status
void ui_scenario_end(void);                      // remove the temporary storage dir
void ui_scenario_draw_background(void);          // after the frame's glClear, instead of the world
int  ui_scenario_draws_preview(void);            // state.preview: first-person weapon + class preview
void ui_scenario_preview_backdrop(Client*);      // no state.preview: only the class preview's dimmed backdrop
#endif
// END ui_scenario block

// ============================================================================ controls
// BEGIN ui_controls block (client/src/ui/ui_controls.c): Krunker's keybind table
// (CONTROLS.binds, DOM keyCodes; mouse 10000 + which, wheel 20000) persisted under its
// localStorage keys (cont_*), the Change Controls window (Krunker window 7) with its
// "Press any Key" capture, and the bound-key state main.c samples for the game.

// Bind ids = Krunker's changeCont/unbindCont ids.
enum {
    UI_CONTROL_FORWARD = 0, UI_CONTROL_BACKWARD = 1, UI_CONTROL_LEFT = 2, UI_CONTROL_RIGHT = 3,
    UI_CONTROL_JUMP = 4, UI_CONTROL_CROUCH = 5, UI_CONTROL_MELEE = 6, UI_CONTROL_SWAP = 7,
    UI_CONTROL_RELOAD = 8, UI_CONTROL_SPRAY = 9, UI_CONTROL_AIM = 10, UI_CONTROL_CHAT = 11,
    UI_CONTROL_VOICE = 12, UI_CONTROL_PRIMARY = 13, UI_CONTROL_INSPECT = 14, UI_CONTROL_PLAYER_LIST = 15,
    UI_CONTROL_INTERACT = 16, UI_CONTROL_DROP = 17, UI_CONTROL_STREAK0 = 18, /* 18..22 */
    UI_CONTROL_INTERACT_SEC = 23, UI_CONTROL_WEAPON_VIS = 24, UI_CONTROL_SHOOT = 25, UI_CONTROL_BOARD = 26,
    UI_CONTROL_TOGGLE0 = 27, /* 27..32 */ UI_CONTROL_EQUIP = 33, UI_CONTROL_PREMIUM0 = 34, /* 34..37 */
    UI_CONTROL_COUNT = 38
};

// Input-related settings ClientSettings does not carry (written by client_apply_setting).
typedef struct {
    float fps_fov;          // fpsFOV, degrees (first-person weapon camera)
    int scroll_dir;         // scrollDir: 1 normal, 0 reversed wheel weapon swap
    int show_weapon, show_weapon_sec, show_weapon_mel; // showWeapon* (first-person model)
    int shw_chat;           // shwChat: 0 all, 1 on focus, 2 off
    float master_volume, voice_volume, asset_volume; // sound, voiceVolume, assetVolume (stored)
    int slid_cont;          // slidCont ("Non FPS Sliding", default on): sent in ENT (ent[15]), applies on the next spawn
} UiInputSettings;

void ui_controls_load(void);                     // binds + toggle sets from storage
void ui_controls_reset(void);                    // resetSettings: drop cont_*, defaults
int  ui_controls_bind(int id);                   // DOM keyCode / 10000+which / 20000, -1 unbound
int  ui_controls_dom_key(int glfw_key);          // GLFW key -> DOM keyCode, -1 unknown
const char *ui_controls_key_name(int code, char *buf, size_t n); // aU.getKeyName
char *ui_controls_window_html(void);             // windows[6].gen() (malloc'd)
void ui_controls_change(int id, int no_render);  // changeCont
void ui_controls_unbind(int id, int no_render);  // unbindCont
void ui_controls_change_set(int id, const char *setting); // changeContSet
cJSON *ui_controls_export_json(void);            // CONTROLS.export() (caller frees; NULL = all default)
void ui_controls_import_json(const cJSON *controls); // settings.import's "controls" object
int  ui_controls_capturing(void);                // waiting for "Press any Key"
int  ui_controls_take_swallowed_key(void);       // web: the capture consumed the current keydown
// GLFW events from main.c. Return 1 when consumed (main.c then does nothing else).
int  ui_controls_key_event(Client*, int key, int scancode, int action, int mods);
int  ui_controls_mouse_event(Client*, int button, int action, int in_game);
int  ui_controls_scroll_event(Client*, double dy, int in_game); // in game: 1 = the wheel is bound (no weapon swap)
// Chat (#chatInput)
void ui_controls_chat_focus(int focused);        // focusChat(on)
// document.activeElement is an INPUT / TEXTAREA / SELECT / contenteditable (any page), or the
// chat / terminal input owns the keyboard: no hotkey may fire (mods.ts isEditableTarget).
int  ui_text_input_focused(void);
int  ui_controls_chat_focused(void);
int  ui_controls_chat_enter(Client*);            // Enter seen by the page in #chatInput
void ui_controls_apply_chat_visibility(void);    // shwChat
// Game side
int  ui_controls_down(int id);                   // the bound key/button is held
int  ui_controls_scoreboard_held(void);          // sBoardKey held (Tab), not while typing
void ui_controls_fill_input(Client*, Input*, float mouse_dx, float mouse_dy);
void ui_controls_end_frame(void);
void ui_controls_reset_keys(void);               // CONTROLS.resetKeys (menu opened / lock lost)
int  ui_controls_scroll_dir(void);               // scrollDirM: 1 / -1
float ui_controls_fps_fov(void);                 // fpsFOV in radians
int  ui_controls_weapon_visible(const Client*);  // showWeapon* for the weapon in hand
UiInputSettings *ui_controls_settings(void);
// Controller: CONTROLS.cntrlInput (the Controller settings) and the controls.js entry
// points the gamepad module (client/src/gamepad.c) calls.
typedef struct {
    float sens_x, sens_y;   // sensitivityXCntrl / sensitivityYCntrl x 5
    float aim_x, aim_y;     // aimSensitivityXCntrl / aimSensitivityYCntrl x 5 (with DNCv5: right mouse)
    float dead_zone_l, dead_zone_r, trigger_thres; // deadZoneL, deadZoneR, triggerThres
    int invert, vibration, grad_speed;              // invertYCntrl, vibration, gradSpeed
} UiControllerInput;
UiControllerInput *ui_controls_cntrl_input(void);
int  ui_controls_enabled(const Client*);         // CONTROLS.enabled: the game owns the pointer
void ui_controls_controller_key(Client*, int id, int down); // Q(null, binds[id]) / U(null, binds[id])
void ui_controls_fake_mouse(Client*, int type, float dx, float dy); // fakeMouse(0 down / 1 up / 2 move, "cntrlInput")
void ui_controls_toggle(Client*, int on);        // CONTROLS.toggle
int  ui_controls_input_is_cntrl(void);           // inputType == "cntrlInput"
void ui_controls_look_frame_end(void);           // updateMouseInp's a4 = a5 = 0
void ui_controls_set_speed_limit(float limit);   // speedLmt (gradSpeed)
float ui_controls_speed_limit(void);
void ui_controls_touch_shoot(int on);            // #mobileUI shoot buttons: UCUz1 (ui_mobile.c)
// END ui_controls block

// BEGIN ui_loadout block (client/src/ui/ui_loadout.c): Change Loadout + item pickers,
// K.P.D popup, Changelog (Krunker v3.1.1 guest state).
char *ui_loadout_generate(int id, const char **header); // windows 3/9/15/16/17/20/26/31/32/34/35/38/39; NULL other ids
char *ui_loadout_gen(void);                      // windows[2].gen()
void ui_loadout_select(const char *fn, int value); // selectHat/Back/Waist/Melee/Dye/Face/Attachment/Reticle/Spray
void ui_loadout_select_skin_color(int index);   // selectSkinColor
void ui_loadout_select_stat(int index);         // selectStat
void ui_loadout_skin_selector(int weapon, int secondary); // skinSelector
void ui_loadout_select_skin(int index, int weapon); // selectSkin
void ui_loadout_action(int action);              // windows[2].action
void ui_loadout_toggle_log(HtmlElem obj);        // windows[34].toggleLog
int  ui_loadout_secondary(int class_index);     // selected secondary weapon id (secondaryInd, default 2), -1 = class has none
int  ui_loadout_skin_color(void);                // selected skin colour as 0xRRGGBB, -1 = class colour
void ui_loadout_cosmetics(PlayerCosmetics *out); // skin colour + hat/back/waist/face/dye picks for ENT and the preview
void ui_loadout_ent_look(int class_index, PlayerCosmetics *cosmetics, PlayerSkins *skins); // ENT: cosmetics + weapon/melee skins, Random resolved to owned items
void ui_loadout_preview_skins(int class_index, PlayerSkins *out); // the class preview's weapon/melee skins (Random shows none)
int  ui_loadout_spray(void);                    // ENT ent[1]: the selected spray (sprayindex), Random resolved, unowned -> 0
void ui_loadout_class_bubble(int index);         // selectClass speech line
void ui_loadout_show_police_pop(void);           // shoPolicePop
void ui_loadout_hide_police_pop(void);           // hidePolicePop
void ui_loadout_clear_pops(void);                // clearPops
int  ui_police_active(void);                     // Officer Buddy is in the class preview
void menu_idle_reset(void);                      // menu_ui.c: CONTROLS.idleTimer = 0 (any key, window, menu toggle)
float menu_preview_backdrop_alpha(void);         // menu_ui.c: class preview backdrop (0.5, 0.875 under the K.P.D popup)
void ui_games_switch_tab(int tab);               // ui_windows.c: windows[9].switchTab (Custom Games tabs)
// ui_dialog.c: window.confirm/prompt/alert (web: the browser's; native: the page's
// #confPop popup) and external links (web: window.open; native: system browser, http(s) only).
#define UI_SETTINGS_IMPORT_MAX 32768
typedef void (*UiDialogFn)(int accepted, const char *text, void *user);
void ui_confirm(const char *text, UiDialogFn fn, void *user);
void ui_prompt(const char *text, UiDialogFn fn, void *user);
void ui_alert(const char *text);
void ui_dialog_answer(int accepted);             // knDialog(1|0)
void ui_dialog_cancel(void);                     // clearPops / #popupBack
int  ui_dialog_prompt_focused(HtmlElem el);      // el is the open prompt's text field
int  ui_resolve_external_url(const char *url, char *out, size_t n); // 0 = not http(s)
void ui_open_external(const char *url);          // openURL / openTab / window.open
// ui_settings.c: settings window header actions
void ui_settings_reset_confirm(void);            // resetSettings(): confirm, Default preset, binds, re-render
void ui_settings_select_preset(int index);       // selectSettingPre(index)
void ui_settings_import_prompt(void);            // importSettings(): prompt + settings.import
int  ui_settings_import_text(const char *text);  // settings.import(text): 1 on success
// END ui_loadout block

// BEGIN ui_bhop block (client/src/ui/ui_bhop.c, ui_notify.c): the v3.1.1 offline build's
// client mod (Krunker_v3.1.1/mod/mods.ts bhop + notify.ts), ported 1:1. The "Developer
// Tools" bind rows and the mods.ts keydown matching live in ui_mods.c (console agent).
enum {
    UI_BHOP_DISABLED = 0, UI_BHOP_SLIDEHOP = 1, UI_BHOP_HOLD_JUMP_CROUCH = 2,
    UI_BHOP_ALWAYS_JUMP = 3, UI_BHOP_HOLD_JUMP = 5 // mods.ts BhopMode (4 = legacy HOLD_JUMP_CROUCH)
};
int  ui_bhop_mode(void);                         // window.bhopMode (BhopMode value)
void ui_bhop_set_mode(int mode);                 // window.bhopMode/bhopEnabled + localStorage "bhop"; no toast
const char *ui_bhop_mode_name(int mode);         // BHOP_MODE_NAMES ("Disabled", "Slidehop (while moving)", ...)
void ui_bhop_toggle(void);                       // toggleBhopMode: next in BHOP_MODE_ORDER, store, notify
const char *ui_bhop_bind(void);                  // window.bhopModeKey: KeyboardEvent.code ("KeyB"), "" = unbound
void ui_bhop_set_bind(const char *code);         // setBhopModeBind: stores krk_bhop_mode_bind
void ui_bhop_fill_input(Client*, Input*);        // patchInputs on this input frame (ui_controls_fill_input tail)
void ui_notify(const char *html);                // notify.ts: top-right #0005 toast, `slide` 2 s
void ui_notify_tick(double now_ms);              // hud_ui.c per frame: steps the notify animation
// client/src/ui/ui_mod_keys.c: the rest of mods.ts' keys. ui_mods.c calls the keydown
// switch after its bind matching (`input` = a text field has focus); 0 = not consumed.
int  ui_mod_keys_keydown(Client*, int glfw_key, int input); // F1 clearLog, F4 "/", F5 reload, F6 editor, T Ctrl TP
int  ui_mod_keys_ctrl_tp_mouse(Client*, int glfw_button, int action); // Ctrl TP canvas mouse wrapper; 1 = taken
// END ui_bhop block

// BEGIN hud_popups block (client/src/ui/hud_popups.c): the game-overlay canvas text popups
// of v3.1.1 (overlay animateText / animateDiv): score "+N", "Checkpoint!", medal banner.
void hud_popups_score(int delta);                // gf(): "5" score popup (bN accumulates while up)
void hud_popups_checkpoint(void);                // gg(): "chp" Checkpoint! (showMedals)
void hud_popups_medals(const NetMedalPacket *medals, int count); // gm(): "6" / "am" medal queue
void hud_popups_assist(void);                    // gl(): "10" Assist medal
void hud_popups_medal_text(const char *text);    // gm([text, null]): a trigger's Broadcast / Send Popup
void hud_popups_on_spawn(void);                  // "start": medal queue cleared, overlay.reset()
void hud_popups_on_init(void);                   // "init": bN = 0
void hud_popups_page_reset(void);                // fresh page: elements gone
void hud_popups_tick(double now_ms, int visible); // per frame: draw + age every popup
// END hud_popups block
void hud_ui_kill_streak(Client*, int uid, int streak); // killStreakMessage ("kst"): "<who> on a N Kill Streak" chat line
