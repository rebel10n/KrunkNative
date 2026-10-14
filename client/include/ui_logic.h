#pragma once
// C UI logic layer over the DOM-ops API (html_dom.h): the C port of Krunker v7.1.4's
// menu and HUD JavaScript (src/index.js, src/libs/UI.js, src/ui/game.js) that drives its
// page (ui/, imported by tools/ui-import) without JavaScript. Contract: doc/html-ui-engine.md; plan: doc/html-engine-plan.md, "C UI
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

// Window ids: Krunker's showWindow(n) ids = windows[n - 1] + 1 (v7.1.4: 1 settings, 2 servers,
// 3 customize, 8 host, 24 join, 48 server finder, ...).
#define MENU_WINDOW_NONE 0
#define MENU_WINDOW_HOST 8
#define MENU_WINDOW_JOIN 24

#define MENU_CLASS_MAX 16
#define MENU_MAP_MAX 64
#define MENU_NAME_MAX 32

// Region label Krunker shows in #menuRegionLabel once it knows the game's region;
// "..." until then. KrunkNative has one server.
#define MENU_REGION_NAME "Local"

#define MENU_CLASS_LOADOUT_MAX 4
typedef struct {
    int index;
    const char *name;    // borrowed from Game.classes / rules_classes()
    const char *weapon;  // primary weapon name, "" when unknown
    int hp;
    char icon[48];       // textures/classes/icon_N.png
    const char *loadout_names[MENU_CLASS_LOADOUT_MAX]; // weaponConfig names of the class loadout
    int loadout_ids[MENU_CLASS_LOADOUT_MAX];           // their weapon ids (the skin each one wears)
    int loadout_count;
    char preview[96];    // textures/previews/weapons/<src>.png of the last loadout weapon ("" none)
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
    float scale_ui;       // 0.7 default (v7 desktop) -> uiCustomScale = 2 - scale_ui
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
    int settings_view;       // windows[0].settingType: 0 "basic" (default), 1 "advanced" (krk_advanced)
    int selected_map;        // Game.maps index picked in the host/games windows, -1 = none

    MenuClass classes[MENU_CLASS_MAX];
    int class_count;
    MenuMap maps[MENU_MAP_MAX];
    int map_count;

    char last_rect[96];      // last previewRect handed to class_preview.c (dedupe key)
    char last_instructions[4096];
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
int  menu_error_screen_shown(void);              // the Lost Connection screen (#instructionsUpdate) is up
void menu_scale_base(void);                      // scaleBase
void menu_send_preview_rect(void);               // sendPreviewRect -> client_class_preview_set_rect
void menu_schedule_rect(void);                   // scheduleRect
void menu_play(void);                            // play
void menu_host_offline(void);                    // hostOffline
void menu_retry_connect(void);                   // retryConnect
void menu_show_connecting(void);                 // CONNECTING... + the connect timeout, no reconnect / seek
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
// Module handlers (ui_handlers.c h_fallback): the inline-handler names one module owns.
// -1 = not ours, else the handler's result (0 = `return false`).
int ui_settings_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc); // ui_settings.c
int ui_controls_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc); // ui_controls.c
int ui_loadout_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc);  // ui_loadout.c
int hud_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc);      // hud_ui.c

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

void  sb_append_replace_all(StrBuf*, const char *tpl, const char *needle, const char *replacement); // tpl.split(needle).join(r)

// windows.json entry of one captured window (tools/ui-import/windows_v7.py)
typedef struct {
    const char *header;      // windowHeader.innerHTML
    int sticky;              // #menuWindow.stickyHeader
    const char *cls;         // #menuWindow class after showWindow ("dark", "stickyHeader dark", "menuWindowF dark")
    const char *style;       // #menuWindow inline style after showWindow (overflow-y, width, max-height)
    const char *label;       // windows[n].label ("settings", "server_browser", ...)
} UiWindowMeta;
int         ui_windows_load(void);                       // ui/windows/windows.json + files; 1 on success
int         ui_windows_reload(void);                     // F5: the templates read again (game_reload.c)
const char *ui_windows_html(const char *key);            // the captured markup of showWindow(key), "" when absent
const UiWindowMeta *ui_windows_meta(const char *key);    // NULL when v7 has no such window
int         ui_settings_view_tab_count(int view);        // windows[0].tabs[basic|advanced].length
const char *ui_settings_view_tab_html(int view, int tab); // the captured windows[0] markup on that tab
const char *ui_settings_view_tab_name(int view, int tab); // "General", "Controls", ...
int         ui_settings_tab_count(void);                 // the advanced view (every setting)
const char *ui_settings_tab_html(int tab);
int         ui_controls_tab_count(void);                 // windows[6].tabs (Change Controls)
const char *ui_controls_tab_html(int tab);               // captured markup (default binds) of that tab
const char *ui_controls_tab_name(int tab);
// windows[id - 1].gen(): malloc'd markup for #menuWindow, header text and stickyHeader
// flag. NULL when the id is not available in KrunkNative.
char *ui_window_generate(int id, const char **header, int *sticky);
void  ui_window_meta(int id, const char **cls, const char **style); // showWindow's #menuWindow class + inline style
void  ui_window_post(int id);                             // WINDOWS[id].post()
// UTILS.assetsUrl(path) of an image the page's own tree holds (ui/img/...: the menu imports,
// tools/ui-import/fetch_season_images.sh): "./" + path, resolved against ui/ like the page's
// own markup (v7's hook.js rewrote assets.krunker.io to its local tree the same way).
void  ui_img_url(const char *path, char *out, size_t n);
const char *ui_window_key(int id);                        // "1".."52"; NULL unknown
const char *ui_map_thumb(const char *name);               // mapThumb()
void        servers_ui_set_host_type(int host_type);      // openHostWindow(false, type): the Host Game window's config.hostTypes index
int         servers_ui_host_type(void);                   // GAME_HOST_CUSTOM unless set
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
int  ui_settings_reload(void);                   // F5: knSettings read again and pushed (game_reload.c); changed count
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
    const char *killer;  // killer alias; NULL/"" = "You Died" (no VS layout)
    const char *weapon;  // killer's weapon (or skin) name shown as [name]
    int level;           // killer class level ("lvl N", at least 1)
    int score;           // +score for the kill (v3 kill card; v7's death screen does not show it)
    int class_index;     // killer class -> textures/classes/icon_N.png
    // v7 death screen (f1975); zero = unknown
    const char *weapon_icon; // killer weapon's weaponConfig.icon ("icon_2") -> img/death-ui/<icon>.png
    int thrown;          // a thrown knife: "Thrown [name]"
    int my_level;        // our class level ("lvl N")
    int stats;           // the life's stats below are set (else the HUD samples time alive / kills / score)
    int time_alive_ms, shots, kills, life_score; // shots < 0 = "Unknown"
    const char *accuracy; // "47%"; NULL = "Unknown"
    // part 2a: the rest of f1975 (NET_PACKET_DEATH_INFO / the killer's and our SPAWN data)
    const char *weapon_color;    // the killer's weapon skin rarity colour ("[name]"), NULL = rgba(255,255,255,0.7)
    const char *killer_card_url; // .death-row-bottom-bg banners: Game.store.skins[playerCardIndex].thumbnail, NULL none
    const char *my_card_url;
    const char *pc_stat_name;    // the killer's player card stat: config.playerCardStats[i].name, NULL = none ("3" [2] null)
    const char *pc_stat_value;
    int report;                  // "[<playerList key>] to Report" (killer level <= 10, signed in, public game)
} HudKillCard;
// v7 io_killPlayer for the local player: #deathUIHolder (f1975) + UI.toggleGameUI(false).
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
// WP-E part 2c: the end screen's Medals tab (NET_PACKET_END_MEDALS: endData.mdls), the Rewards tab
// (matchRewardSummary.skins of completed challenges) and #endChals' completed challenge cards
// (io_challengeCompleted). All reset when the end screen goes (a new game).
void hud_ui_set_end_medals(const NetEndMedalsPacket *packet);
void hud_ui_add_reward_skin(int skin);
void hud_ui_end_refresh(void); // re-render the end screen tab (the Battle Pass bpData came in late)
void hud_ui_end_results_now(const Client*); // --render-scenario endscreen.results: skip config.endAnim
void hud_ui_skin_found(Client*, int uid, int skin); // season-system: NET_PACKET_SKIN_FOUND (v7 "skin" / "skinF")
void hud_ui_found_junk(const char *line);           // NET_CHAT_FOUND_JUNK "You found [x.xx] Junk" (v7 io_foundJunk)
void hud_ui_end_challenge_completed(int icon, const char *name);
void hud_ui_render_team(const Client*);     // the player card's team type / name (io_updateTeam, isYou)
int  hud_ui_end_screen_up(void);            // END to INIT (title animation or results): no menu over it unless toggled
int  hud_ui_end_screen_interactive(void);   // end-screen results up: pointer free, mouse goes to the page
int  hud_ui_toggle_end_screen(Client*);     // UI.toggleEndScreen: results <-> menu (Escape, controller Start); 1 = acted
// The HUD's visibility pass (UI.toggleGameUI / toggleControlUI per the current state: playing,
// dead, menu, end screen) runs again next frame, and every piece that follows its layout epoch
// re-applies itself after it. A state owner that gives the HUD back without a menu / pointer
// change (spectating ended, a speed-run mode left, a new INIT) calls this instead of
// restoring the elements it hid.
void hud_ui_relayout(void);
// INIT (a new session, round, map or mode): v7 resetGameSessionState's toggleGameUI(true) (the
// relayout above) and f1904(Game.isComp) (the comp lobby's menu or the default one).
void hud_ui_on_init(Client*);
void hud_ui_on_config(Client*); // a GAME_CONFIG applied (it may follow the INIT): f1904 again with it
char *hud_ui_player_list_html(void);        // windows[22].gen(): "Player List" (pListKey, showWindow(23)); malloc'd
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
void hud_ui_add_chat_channel(const char *name, const char *msg, const char *color, int channel); // WP-E: a v7 "ch" line on its channel (-1 all)
void hud_ui_clear_chat(void);                    // empty #chatList, no join lines seeded
// hud_ui_add_killfeed with explicit "You" flags and the row's age (into its 4 s life). A NULL
// killer (not "You") is a self-kill: weapon + victim only. Old Scoreboard puts the row in #chatList.
// `streak` >= 0: a streak kill (v7 info.stk, the Nuke's): its img/streaks icon replaces the weapon's.
void hud_ui_add_killfeed_ex(Client*, const char *killer, int killer_is_you, const char *victim, int victim_is_you,
                            const char *weapon_icon, int is_headshot, int streak, double age_ms);
void hud_ui_hit_elapsed(Client*, int kill, double elapsed_ms); // hud_ui_hit `elapsed_ms` ago

// Scenario overrides (NULL clears): the `state.hud` payload replaces the values the HUD
// samples from the game; `state.scoreboard` rows replace the local-player-only board.
void hud_ui_set_override(const cJSON *hud);
void hud_ui_set_override_scoreboard(const cJSON *rows);
// WP-E: NET_PACKET_LEADER_STATS (v7 "7" leaderStats columns, board order); NULL forgets it (new round).
void hud_ui_set_leader_stats(const NetLeaderStatsPacket *packet);
void hud_ui_set_clock(double now_s);             // HUD clock (hit marker, 20 Hz pacing); < 0 = GLFW clock
const char *hud_ui_override_timer(void);        // scenario hud.timerText (also #specTimer), NULL none
double hud_ui_now_ms(const Client*);            // the HUD clock in ms (hud_ui_set_clock or GLFW)
// World point -> #uiBase fraction (0..1, y down) through the scene camera; 0 outside the frustum.
int hud_ui_project(const Client*, float x, float y, float z, double *sx, double *sy);

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
    // WP-C: v7.1.4 binds.reset / binds.resetLast (input[13]); KrunkNative ids, stored as cont_resetKey /
    // cont_resetLastKey like v7
    UI_CONTROL_RESET = 38, UI_CONTROL_RESET_LAST = 39,
    // KrunkNative addition: Reconnect (unbound by default, cont_reconnectKey; F5 until the Reload
    // Game bind took it), the console's `reconnect`
    UI_CONTROL_RECONNECT = 40,
    // KrunkNative addition: the Object Manager (F1, cont_objectManagerKey) toggles the spawn menu
    // (client/src/ui/object_manager_ui.c); the slot held F1's earlier Game Settings bind
    UI_CONTROL_OBJECT_MANAGER = 41,
    // KrunkNative additions: the Object Manager's Undo (U, cont_omUndoKey) and Remove at the
    // crosshair (Delete, cont_omRemoveKey), in game
    UI_CONTROL_OM_UNDO = 42, UI_CONTROL_OM_REMOVE = 43,
    // KrunkNative addition: the Physics Gun's rotate key (E, cont_omRotateKey): held while carrying,
    // the mouse turns the object instead of the view (client/src/ui/om_build.c)
    UI_CONTROL_OM_ROTATE = 44,
    // KrunkNative additions: the Tool Gun's grid step down / up (PageDown / PageUp, cont_omSnapDownKey / cont_omSnapUpKey)
    UI_CONTROL_OM_SNAP_DOWN = 45, UI_CONTROL_OM_SNAP_UP = 46,
    // KrunkNative addition: Third Person (cont_thirdPersonKey, unbound by default) toggles the
    // over-the-shoulder camera like the console's thirdperson / firstperson (third_person.h)
    UI_CONTROL_THIRD_PERSON = 47,
    // KrunkNative addition: Reload Game (F5, cont_gameReloadKey): the full reload (game_reload.h),
    // the console's `reload`
    UI_CONTROL_GAME_RELOAD = 48,
    // KrunkNative addition: the Tool Gun's Properties key (I, cont_omPropsKey): the selection's fields
    UI_CONTROL_OM_PROPS = 49,
    UI_CONTROL_COUNT = 50
};

// Input-related settings ClientSettings does not carry (written by client_apply_setting).
typedef struct {
    float fps_fov;          // fpsFOV, degrees (first-person weapon camera)
    int scroll_dir;         // scrollDir: 1 normal, 0 reversed wheel weapon swap
    int show_weapon, show_weapon_sec, show_weapon_mel; // showWeapon* (first-person model)
    int shw_chat;           // shwChat: 0 all, 1 on focus, 2 off
    float master_volume, voice_volume, asset_volume; // sound, voiceVolume, assetVolume (stored)
    int slid_cont;          // slidCont ("Non FPS Sliding", default on): sent in ENT (ent[15]), applies on the next spawn
    int chall_mode;         // challMode ("Challenge Mode", default off): sent in ENT V8 (ent[7]), applies on the next spawn
} UiInputSettings;

void ui_controls_load(void);                     // binds + toggle sets + quick chat messages from storage
void ui_controls_reset(void);                    // resetSettings: drop cont_*, defaults
int  ui_controls_bind(int id);                   // DOM keyCode / 10000+which / 20000, -1 unbound
int  ui_controls_bind_alt(int id);               // the alternate key (v7 ControlBind.altVal), -1 unbound
int  ui_controls_v7_bind(const char *bind, int index, int alt); // CONTROLS.binds[bind][index].val / altVal (index -1: scalar)
int  ui_controls_dom_key(int glfw_key);          // GLFW key -> DOM keyCode, -1 unknown
const char *ui_controls_key_name(int code, char *buf, size_t n); // UTILS.getKeyName
int ui_controls_v7_val(const char *bind, int index); // CONTROLS.binds.<bind>[index].val (index -1: scalar), -1 none
char *ui_controls_window_html(void);             // windows[6].gen() (malloc'd)
void ui_controls_change(const char *bind, int alt, int index);    // changeCont(bind, alt, index)
void ui_controls_unbind(const char *bind, int alt, int index);    // unbindCont
void ui_controls_reset_bind(const char *bind, int alt, int index); // resetCont
void ui_controls_change_set(int index, const char *setting);      // changeContSet(0..5, setting)
void ui_controls_change_msg(int index, const char *text);         // changeContMsg(0..5, text)
void ui_controls_switch_tab(int tab, int force);                  // windows[6].switchTab
cJSON *ui_controls_export_json(void);            // CONTROLS.export() (caller frees; NULL = all default)
void ui_controls_import_json(const cJSON *controls); // settings.import's "controls" object
int  ui_controls_capturing(void);                // waiting for "Press any Key"
int  ui_controls_take_swallowed_key(void);       // web: the capture consumed the current keydown
int  ui_controls_take_game_key(void);            // web: the game owned the current key event (preventDefault it)
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
// WP-C netRate: a frame that samples no input frame still turns the look (CONTROLS.updateMouseInp
// runs every frame); the keys and one-frame events wait for the next sampled frame.
void ui_controls_fill_look(Client*, Input*, float mouse_dx, float mouse_dy);
void ui_controls_end_frame(void);
void ui_controls_reset_keys(void);               // CONTROLS.resetKeys (menu opened / lock lost)
void ui_controls_release_focus(void);            // the game took the input back: blur #chatInput / a page text field
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

// BEGIN build_ui block (client/src/ui/build_ui.c): the held weapon's game-overlay canvas items
float build_ui_charge_progress(const Player *me); // interMProg: chargeTime / (chrgTime || throwCharge), 0 none
void build_ui_render(Client*);                    // per frame, after the HUD: the charge bar and the build wheel
int  build_ui_shown(void);                        // the last build_ui_render drew something (tests)
void build_ui_scenario(double prog, int over);    // --render-scenario state.charge: this charge bar (prog < 0: off)
int  build_ui_scenario_active(void);              // a scenario charge bar is set (main.c draws it in scenario mode)
void build_ui_fini(void);                         // its canvas (GL)
// END build_ui block

// BEGIN hud_objectives block (client/src/ui/hud_objectives.c): the objective modes' game-overlay
// canvas items (flag pings, the Hardpoint Capture / Next marker, CONTESTED, Domination zones and
// the capture bar)
void hud_objectives_render(Client*);              // per frame, after the HUD
int  hud_objectives_shown(void);                  // the last hud_objectives_render drew something (tests)
void hud_objectives_fini(void);                   // its canvas (GL)
// END hud_objectives block

// BEGIN object_manager_ui block (client/src/ui/object_manager_ui.c): the KrunkNative Object Manager
// (F1 spawn menu, shared/include/object_manager.h). Not a Krunker window: KrunkNative's own windows
// take ids from KN_WINDOW_BASE, above every Krunker page's windows[] (v10 uses 53 customize and
// 54 title_selection; doc/v10/ui.md). Builds before the ruleset seam had it at 54.
#define KN_WINDOW_BASE 100
#define OBJECT_MANAGER_WINDOW (KN_WINDOW_BASE + 0)
#define TURF_WINDOW (KN_WINDOW_BASE + 1)    // v10 Turf Wars (turf_ui.c; v10 shows it in a popup)
// KN_WINDOW_BASE + 2: SERVERS_WINDOW_CONNECT (backend_ui.h), the menu's Connect window
void object_manager_ui_toggle(Client*);          // the F1 bind: open (pointer freed) / close (pointer back)
int  object_manager_ui_escape(void);             // Escape on the window: 1 = back to the game
int  object_manager_ui_search_focused(void);     // its search box has the keyboard (F1 still toggles)
int  object_manager_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc); // knOm*; -1 not ours
char *object_manager_window_gen(int id, const char **header); // NULL for other ids
int  object_manager_events_handle(Client*, int type, const void *payload, uint16_t length); // packets 251-253
void object_manager_ui_reset(void);              // a new server (INIT): its caps are unknown again
void object_manager_ui_tick(Client*);            // per frame: me->build_free from the caps (asks once with a Build Tool)
void object_manager_ui_pick_weapon(int weapon_id, int drop); // a Weapons-tab left (add / take out) or right (drop) click
void object_manager_ui_pick_tool(int tool);      // a Tools-pane pick: the Tool Gun's tool, the Tool Gun in hand
void object_manager_ui_key(Client*, int action); // the Undo / Remove binds in game: OM_ACTION_UNDO / OM_ACTION_REMOVE_AIM
void object_manager_ui_chat_sent(const char *text); // a chat / console line went out: `/give <weapon>` equips it once given
void ui_controls_select_slot(int slot);          // ui_controls.c: equip loadout slot `slot` with the next input frame
int  object_manager_ui_tool(void);               // the Tool Gun's tool (ObjectManagerTool) picked in the Tools pane
void object_manager_ui_pick_physgun(void);       // the Tools pane's Physics Gun tile: in hand (added first when not carried)
int  object_manager_ui_caps(void);
int  ui_controls_take_slot(void);                // ui_controls.c: the pending slot pick (Input.select_slot) for a frame the controls do not fill               // OM_CAP_* of this server's last answer, 0 none yet
void object_manager_ui_snap_changed(void);       // the grid step changed (keys, console): the pane's field and a status line
// KrunkNative Physics Gun and btools (client/src/ui/om_build.c)
#define OB_SNAPS 4                               // the old grid steps (1 / 2 / 5 / 10 units): kn_omSnap's index
#define OB_SNAP_MIN 0.1f                         // the btools' grid step: any value in OB_SNAP_MIN..OB_SNAP_MAX, or 0 (off)
#define OB_SNAP_MAX 100.0f
void om_build_input(Client*, Input*, float prev_pitch, float prev_yaw, int sampled); // main.c: per frame, the pointer locked
void om_build_render(Client*);                   // main.c: after the world (the beam, the box, the handles)
void om_build_fini(void);
float om_build_snap(void);                       // the grid step (units), 0 off (kn_omSnapStep)
float om_build_set_snap(float step);             // clamped to OB_SNAP_MIN..OB_SNAP_MAX (<= 0: off); the step set
float om_build_step_snap(int dir);               // the snap keys: the next common step up (1) / down (-1)
void om_build_snap_text(char *out, size_t size); // "5", "0.25", "off"
int  om_build_drop(void);                        // the Physics Gun lets go: 0 frozen in place, 1 down onto the surface
void om_build_set_drop(int on);
int  om_build_grabbing(void);
void om_build_refused(int status);               // a refused edit: the grab / drag ends
void om_build_reset(void);                       // a new map / connection
int  om_build_handle_look(Client*, int index, float *pitch, float *yaw); // input scripts: the look onto a handle
int  om_build_props_key(Client*);                // the Properties key: the selection's (or the crosshair's) fields; 1 opened
int  om_build_selection(void);                   // the btools selection's uid, -1 none
void object_manager_ui_open_props(Client*, int uid); // the window's Properties panel on object `uid` (opened over the game)
int  object_manager_ui_props_uid(void);          // the object the Properties panel shows, -1 none
void object_manager_ui_pick_entry(int tab, int item); // an Objects / Entities / NPCs tile click without the window
// END object_manager_ui block

// BEGIN v10_news block (client/src/ui/v10_news.c): Krunker v10.0.2's What's New (windows[34]
// Changelog over Krunker Box Office's kn-versions.txt, the menu's #updateAd), Guide (openGuide)
// and Community & Events (openCommunityMenu, the brain's /creators/events and /social/news).
char *v10_news_changelog_gen(void);              // windows[34].gen() (showWindow(35))
void v10_news_toggle_log(HtmlElem obj);          // windows[34].toggleLog
const char *v10_news_version(void);              // config.gameVersion: the changelog's newest update
void v10_news_open_guide(const char *section);   // window.openGuide(section); NULL = "how-to-play"
void v10_news_open_community(void);              // window.openCommunityMenu()
void v10_news_setup(void);                       // the page is up: the v10 menu items' actions
void v10_news_tick(void);                        // per frame: the What's New item
int  v10_news_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc); // -1 not ours
void v10_news_scenario(const cJSON *community);  // --render-scenario state.community
// END v10_news block

// BEGIN ui_loadout block (client/src/ui/ui_loadout.c): Change Loadout + item pickers,
// K.P.D popup (Krunker v3.1.1 guest state).
char *ui_loadout_generate(int id, const char **header); // windows 3/9/15/16/17/20/26/31/32/34/35/38/39; NULL other ids
char *ui_loadout_gen(void);                      // windows[2].gen()
void ui_loadout_select(const char *fn, int value); // selectHat/Back/Waist/Melee/Dye/Face/Attachment/Reticle/Spray
void ui_loadout_select_skin_color(int index);   // selectSkinColor
void ui_loadout_select_stat(int index);         // selectStat
void ui_loadout_skin_selector(int weapon, int secondary); // skinSelector
void ui_loadout_select_skin(int index, int weapon); // selectSkin
void ui_loadout_action(int action);              // windows[2].action
void ui_loadout_tick(void);                      // the open picker's virtual card grid follows the scroll (ui_vgrid.h)
int  ui_loadout_secondary(int class_index);     // selected secondary weapon id (secondaryInd, default 2), -1 = class has none
int  ui_loadout_streak(int slot);                // streakIndex1..3 (data/streaks.js index for slot 0..2), -1 none
int  ui_loadout_skin_color(void);                // selected skin colour as 0xRRGGBB, -1 = class colour
// What a player of `class_index` wears, from the Change Loadout picks: the one source of the ENT
// (main.c, for_ent 1) and the menu's class preview (class_preview.c, for_ent 0), so the preview
// shows what the first-person viewmodel and the other players will. The two differ only where
// v7.1.4 does: the ENT resolves Random (-2) to an owned item (getRandomOwnedSkin), the preview shows
// none for it (updateClassPreview's `== -2 ? -1`). Store items show only with an inventory (signed
// in, or offline: every skin), like the Loadout / Customize tabs draw them (a guest's saved picks
// are stripped by the server anyway). cosmetics are routed (cosmetics_route); attach_index is v7's
// `forceAttach ? 0 : attachIndex` (cosmetics_attach_index).
typedef struct {
    PlayerCosmetics cosmetics;     // skin colour + hat / body / waist / face / dye
    PlayerCosmeticsV7 cosmetics_v7; // head / back / shoe / pet / wrist / charms / card (badge: the server's)
    PlayerSkins skins;             // primary / secondary weapon skins, melee
    int secondary_index;           // setClass's selectedSecondaryIndex (ui_loadout_secondary)
    int attach_index;              // the sight: 0 Reflex, 1 Acog, -1 none
} LoadoutLook;
void ui_loadout_look(int class_index, int for_ent, LoadoutLook *out);
// The store skin the Loadout tab and the menu class box show on `weapon` (skinsLoadout[weapon] with an
// inventory), -1 for none / Random.
int  ui_loadout_weapon_skin_shown(int weapon);
int  ui_loadout_spray(void);                    // ENT ent[1]: the selected spray as a v7 store index, Random resolved, unowned -> 2482
int  ui_loadout_spray_wheel(int *out, int max); // WP-E: the spray wheel's store indices (owned sprays, then the equipped one)
void ui_loadout_class_bubble(int index);         // selectClass speech line
void ui_loadout_switch_tab(int tab);             // windows[2].switchTab(tab, true): Class / Loadout / Customize / Other
void ui_loadout_select_tab(int tab);             // the same from selectClass: re-renders only an open window 3
void ui_loadout_show_police_pop(void);           // shoPolicePop
void ui_loadout_hide_police_pop(void);           // hidePolicePop
void ui_loadout_clear_pops(void);                // clearPops
void ui_loadout_reset_pops(void);                // resetPops (the popup holder stays up)
int social_estimate_value(int skin);             // data/prices.js prices.estimateValue (0 unknown)
int  ui_police_active(void);                     // Officer Buddy is in the class preview
void menu_idle_reset(void);                      // menu_ui.c: CONTROLS.idleTimer = 0 (any key, window, menu toggle)
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
void ui_mod_keys_reload(void); // F5 (game_reload.c): Ctrl TP read again from storage
// END ui_bhop block

// BEGIN hud_popups block (client/src/ui/hud_popups.c): the game-overlay canvas text popups
// of v3.1.1 (overlay animateText / animateDiv): score "+N", "Checkpoint!", medal banner.
// v7.1.4 mode HUD (hud_v7modes.c): #roundsDisplay, #krTagHolder, #bhHolder, #speedRunHolder
void hud_v7modes_on_init(void);                                // INIT: initGame's resets
void hud_v7modes_tick(Client*, int on, int layout_epoch);     // per frame (client_hud_html_tick)
void hud_v7modes_format_time(double ms, int with_ms, char *out, size_t size); // UTILS.getTime(t, ms, h, 1000, d, 2)
void hud_popups_score(int delta);                // gf(): "5" score popup (bN accumulates while up)
void hud_popups_checkpoint(void);                // gg(): "chp" Checkpoint! (showMedals)
void hud_popups_medals(const NetMedalPacket *medals, int count); // gm(): "6" / "am" medal queue
void hud_popups_assist(void);                    // gl(): "10" Assist medal
void hud_popups_medal_text(const char *text);    // gm([text, null]): a trigger's Broadcast / Send Popup
void hud_popups_on_spawn(void);                  // "start": medal queue cleared, overlay.reset()
void hud_popups_on_init(void);                   // "init": bN = 0
void hud_popups_page_reset(void);                // fresh page: elements gone
void hud_popups_tick(double now_ms, int visible); // per frame: draw + age every popup
void hud_popups_set_comp(int comp);              // Game.isComp: io_addMedal shows nothing
// map-logic WP-H zombies HUD (hud_zombies.c): #powerUpHolder timers, the finiteAmmo reserve in
// #ammoMax / #reloadMsg, #roundMessage. Every frame after client_hud_html_tick().
void hud_zombies_tick(Client*);
// END hud_popups block
void hud_ui_nuke_flash(void); // io_nukeUpdate(1): the white #imgFlash, fading out over 500 ms
void hud_ui_nuke_flash_step(float delta); // the game loop's fade (gameTimeDiff, seconds here)
void hud_ui_kill_streak(Client*, int uid, int streak); // killStreakMessage ("kst"): "<who> on a N Kill Streak" chat line
// KrunkScript game.ui hideDefault() (v7): hides #gameUIHider and #fullMenHider (0 = shows them
// again). #customGUIHolder follows #inGameUI's visibility and its children are the script's.
void hud_ui_hide_default(int hidden);
void hud_ui_toggle_whole_ui(int on);             // UI.toggleWholeUI: forceHidden on #fullMenHider/#customGUIHolder/#gameUIHider
int  hud_ui_chat_channel(void);                  // #chatSwitch channel (v7 vLN02023): 0 global, 1 team
