#pragma once
// Krunker Strike's own UI: FRVR's Krunker Strike 1.37.2 mobile page (ui/strike/, provenance in
// ui/strike/README.md), shown instead of Krunker v10's ui/index.html when the process flavor is
// Strike (shared/include/flavor.h: client_bin --flavor strike, KN_FLAVOR=strike, a strike build;
// on the web the page strike/index.html). FRVR renders that UI with Mithril components from its app
// bundle; here the components' markup lives in ui/strike/screens/*.html (transcribed from the
// bundle and checked against the sandbox captures in doc/ref/krunker-strike-1.37.2/) and their
// behaviour is C over the DOM-ops API (html_dom.h), like the v10 page:
//
//   client/src/ui/strike_ui.c      page + screen stack (FRVR's Pt/kt/nu router, yz()), the loading
//                                  screen (vb / fz: curtains, cog, logo, bar), templates, tick
//   client/src/ui/strike_menu.c    main menu, class selection, tap to play, settings, meta screens
//   client/src/ui/strike_hud.c     the in-game UI (H5 / IN_GAME_UI): health, ammo, timer, kill feed,
//                                  scoreboard, kill card, end screens
//   client/src/ui/strike_touch.c   the mobile controls (a5: joystick zones, control buttons): mouse
//                                  natively, touch on the web (always the mobile version)
//
// The Strike page is always THE MOBILE VERSION (FRVR's window._isMobile / useMobileControls true on
// every platform): no pointer lock, touch controls, a 640x360 base layout scaled to the window.
// A base (FLAVOR_BASE) client never calls into these modules: ui/index.html is untouched.
#include <client.h>
#include <html_dom.h>
#include <ui_logic.h> // HudKillCard
#include <cJSON.h>

// 1 when this process shows the Strike page (flavor Strike and the HTML UI available).
int strike_ui_active(void);
// The page tree for main.c: "<ui>/strike/" under the flavor, else base_dir unchanged (malloc'd).
char *strike_ui_page_dir(const char *base_dir);

// menu_ui.c: client_menu_tick() calls these instead of the v10 page's setup / per-frame updates
// while strike_ui_active(). dom_init runs once the page exists (#frvrMenuMobileHolder).
int  strike_ui_dom_present(void);
void strike_ui_dom_init(Client *client);
void strike_ui_tick(Client *client);
// The menu state changed (client_set_menu_open): the screen stack follows (menu <-> game).
void strike_ui_menu_open(Client *client, int open);

// main.c input routing. In game (menu closed) every mouse event is the mobile controls' (a touch
// with the mouse, strike_touch.c); with a screen up the page takes it as usual. 1 = consumed.
int  strike_ui_pointer_to_page(void);   // the page takes pointer input now (a menu screen is up)
int  strike_touch_mouse_button(Client *client, int button, int action, float x, float y); // CSS px
int  strike_touch_mouse_move(Client *client, float x, float y);
// Real touches (web: emscripten touch callbacks on the page). phase 0 start, 1 move, 2 end/cancel.
int  strike_touch_event(Client *client, int phase, long id, float x, float y);
// The frame's look delta from the aim zone / aim joystick (FRVR fakeMouse(2, dx, dy, "mblInput")),
// in CSS px of finger travel; 0 when none. main.c drops raw cursor travel under the Strike page.
int  strike_touch_take_look(float *dx, float *dy);
void strike_touch_tick(Client *client);  // per frame: the controls' DOM follows the game state
void strike_touch_reset(void);           // release everything held (menu opened, death, end)

// ---- strike_ui.c internals shared by the strike_*.c modules
// The screen stack (FRVR's Pi[]): each screen is one template rendered into #frvrMenuMobileHolder.
typedef enum {
    STRIKE_SCREEN_NONE = 0,
    STRIKE_SCREEN_MAIN_MENU,
    STRIKE_SCREEN_CLASS_SELECTION,
    STRIKE_SCREEN_TAP_TO_PLAY,
    STRIKE_SCREEN_SETTINGS,
    STRIKE_SCREEN_CUSTOMIZATION,
    STRIKE_SCREEN_STORE,
    STRIKE_SCREEN_SPIN,
    STRIKE_SCREEN_MISSIONS,
    STRIKE_SCREEN_BATTLEPASS,
    STRIKE_SCREEN_REWARD_TO_CLAIM,
    STRIKE_SCREEN_CREATE_MATCH,
    STRIKE_SCREEN_CUSTOMIZE_CONTROLS,
    STRIKE_SCREEN_LEAN_END,
    STRIKE_SCREEN_MATCH_DETAILS,
    STRIKE_SCREEN_LEADERBOARD_PLAYERS,
    STRIKE_SCREEN_POPUP,
    STRIKE_SCREEN_PROFILE,
    STRIKE_SCREEN_COUNT
} StrikeScreen;

const char *strike_screen_id(StrikeScreen s);     // FRVR's view id ("MAIN_MENU", ...)
StrikeScreen strike_screen_top(void);
int  strike_screen_shown(StrikeScreen s);
void strike_screen_push(StrikeScreen s);          // Pt(view): no-op when already on the stack
void strike_screen_pop(void);                     // kt(): back
void strike_screen_replace(StrikeScreen s);       // nu(view): the stack becomes [s]
void strike_screen_clear(void);                   // in game: nothing but the in-game UI
HtmlElem strike_screen_root(StrikeScreen s);      // the screen's root element, 0 when not shown
void strike_screen_rerender(StrikeScreen s);      // g.redraw() for one screen: template again

// FRVR's loading screen (vb + fz): Fm(Al, {info}) shows it, RA() completes it (curtains open
// after max(0, 1500 ms - shown) then it goes 700 ms later). no_anim: noEnterAnimation.
void strike_loading_show(const char *info, int no_anim);
void strike_loading_complete(void);
int  strike_loading_shown(void);

// Templates: ui/strike/screens/<name>.html with {{key}} placeholders (strike_ui.c). The result
// is malloc'd; values are HTML-escaped unless the key starts with '!' (raw markup).
typedef struct { const char *key; const char *value; } StrikeVar;
char *strike_template(const char *name, const StrikeVar *vars, int count);

// Module hooks called by strike_ui.c.
void strike_menu_install(void);                   // inline handlers of the menu screens
void strike_menu_render(StrikeScreen s, HtmlElem root); // after a screen's markup is inserted
void strike_menu_tick(Client *client);
void strike_menu_action(const char *name);         // strikeAction(name) (tests: pageActions / the e2e)
const char *strike_menu_template(StrikeScreen s);  // template file of a menu screen, NULL none
// Game events (client/src/net/game_events.c, beside hud_ui.c's): no-ops unless strike_ui_active().
void strike_hud_killfeed(Client *client, const char *killer, int killer_is_you, const char *victim, int victim_is_you,
                         const Weapon *weapon, int headshot); // killer NULL: a suicide / world death
void strike_hud_death(Client *client, const HudKillCard *card, const Player *killer); // io_killPlayer (us): the kill card
void strike_hud_end(Client *client, int result);   // endGame: 1 victory, 0 defeat, -1 draw
void strike_hud_end_hide(Client *client);          // the next round's INIT
int  strike_hud_end_up(void);                      // the round-end screen is up
// xe.deathDelay after our death: Cg() -> goToPlay, the Strike player respawns without the menu
// (game_events.c client_game_events_tick, instead of reopening the menu)
void strike_hud_respawn(Client *client);
void strike_hud_install(void);
void strike_hud_tick(Client *client);
void strike_hud_reset(void);                       // a new page / session: in-game UI removed

// --render-scenario: state.strike (strike_ui.c strike_ui_scenario lists its keys)
void strike_ui_scenario(Client *client, const cJSON *state);
void strike_hud_scenario(Client *client, const cJSON *state); // killfeed / killcard / end
void strike_menu_scenario(const cJSON *account, const char *name); // state.strike.account (the account box)
