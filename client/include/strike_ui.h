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
//   client/src/ui/strike_meta.c    missions tab, reward card, match details, the rewards track
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
// joystick.active || moveJoystick.active: a look drag or the move stick is held (the aim assist's
// gate, client/src/strike_assist_client.c)
int  strike_touch_sticks_active(void);
void strike_touch_tick(Client *client);  // per frame: the controls' DOM follows the game state
void strike_touch_reset(void);           // release everything held (menu opened, death, end)
// Customize Controls (a5 in Customize mode): the screen's control layer takes the saved layout
// (mobile_layouts), a finger / the left button drags a control there (phase 0 down, 1 move, 2 up;
// CSS px; 1 = used, the page does not see it) and saves it on release; Reset = Ahe().
void strike_touch_customize_apply(HtmlElem layer);
int  strike_touch_customize_pointer(int phase, float x, float y);
void strike_touch_layout_reset(void);
// The input gate (FRVR's controls.isMobile): 1 once a real touch has played on the Strike page, so the
// touch controls are the input. 0 = keyboard and mouse: the desktop reload overlay and the keyboard
// reload hint (strike_hud.c) show, and the touch reload ring stays hidden.
int  strike_touch_input(void);
// $U: the ammo ring's dash offset (percent), its visibility and the "cur / max" label of the held weapon
void strike_touch_ammo_text(char *out, size_t n, int *offset_pct, int *visible);

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
    STRIKE_SCREEN_ITEM_SELECTION, // T5, a Customization category's items (transcribed, strike_menu.c)
    STRIKE_SCREEN_REPORT_PLAYER,  // Hy (strike_social.c)
    STRIKE_SCREEN_POPUP_SUCCESS,  // XA
    STRIKE_SCREEN_POPUP_ERROR,    // p0
    STRIKE_SCREEN_REWARD,         // VC (Es): a reward's reveal (strike_reward.c)
    STRIKE_SCREEN_REWARD_CHOICE,  // NB (QC): pick one of a level's rewards (strike_reward.c)
    STRIKE_SCREEN_INPUT_BLOCKER,  // ws: "Purchasing..." while a purchase is on its way (strike_reward.c)
    STRIKE_SCREEN_COUNT
} StrikeScreen;

const char *strike_screen_id(StrikeScreen s);     // FRVR's view id ("MAIN_MENU", ...)
StrikeScreen strike_screen_top(void);
int  strike_screen_depth(void);                   // Pi.length
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
// The page is still loading and covers the menu: the static boot screen until the mount, then FRVR's loading
// screen while it shows. Escape does nothing then (main.c key_callback).
int  strike_loading_up(void);

// Templates: ui/strike/screens/<name>.html with {{key}} placeholders (strike_ui.c). The result
// is malloc'd; values are HTML-escaped unless the key starts with '!' (raw markup).
typedef struct { const char *key; const char *value; } StrikeVar;
char *strike_template(const char *name, const StrikeVar *vars, int count);
// One "<!-- part NAME -->" section of a template file (a row / card component of the screen).
char *strike_template_part(const char *name, const char *part, const StrikeVar *vars, int count);

// strike_meta.c: the views no sandbox capture exists for, transcribed from the bundle's Mithril
// code (ui/strike/screens/MISSION_TAB_SCREEN, REWARD_TO_CLAIM, MATCH_DETAILS_SCREEN,
// PROGRESSION_TRACK.html; not pixel-verified): the Battle Pass screen's Missions tab, the
// tournament reward card, the round's match details and PROFILE's Rewards Track.
void strike_meta_render(StrikeScreen s, HtmlElem root); // after strike_menu_render
int  strike_meta_action(const char *name);              // 1 = one of its strikeAction()s
void strike_meta_tick(Client *client);
void strike_meta_econ_changed(void);                    // the Strike account changed (strike_econ_listen)
void strike_meta_scenario(Client *client, const cJSON *state); // --render-scenario state.strike keys
int  strike_menu_level(void);                           // the account box's level (strike_menu.c)

// strike_social.c: FRVR's friends UI on kn_brain's social socket (friends event bus, presence filtered
// to Strike games, game invites, reports): the menu friends strip / Friends List (S5) on MAIN_MENU and
// TAP_TO_PLAY, Private Match's invite list (Vhe), INVITE_NOTIFICATION (N5), REPORT_PLAYER (Hy) and the
// POPUP_SUCCESS / POPUP_ERROR alerts after a report.
void strike_social_render(StrikeScreen s, HtmlElem root);
int  strike_social_action(const char *name);
void strike_social_tick(Client *client);
void strike_social_report(int player_uid, const char *name); // Pt(Hy, {player}): the report popup

// strike_reward.c: FRVR's reward flow, transcribed from the bundle's Mithril views (not pixel-verified):
// REWARD (VC / Es: the reward's reveal, its rarity's lights and colour, the stars of an epic or better
// item, one reward after another), REWARD_CHOICE (NB / QC: a pick-one level of the Rewards Track),
// INPUT_BLOCKER (ws: "Purchasing...") and the store's product popup (U2 in the POPUP frame).
// rewards: FRVR's reward objects ({type: "item" | "class", item: {itemId, ...}} | {type: "currency",
// currency, amount} | {type: "feature", feature}); origin: er.* ("spinWheel", "store", "progressionTrack")
typedef void (*StrikeRewardDone)(void *user);
void strike_reward_show(const cJSON *rewards, const char *origin, int heroic, StrikeRewardDone done, void *user);
// Pt(QC, {options, onChoice}): options are item ids; on_choice(user, index) when Submit is pressed
typedef void (*StrikeChoiceFn)(void *user, int choice);
void strike_reward_choice(const char *const *item_ids, int count, StrikeChoiceFn on_choice, void *user);
void strike_reward_blocker(const char *text);          // Pt(ws, {loadingText})
void strike_reward_blocker_hide(void);                 // Tn() == ws && kt()
// U2: the product popup's markup for the POPUP frame's holder (malloc'd, NULL when the product shows none:
// FRVR opens it only for a paid item product of the market); buy_action is its button's strikeAction
char *strike_product_popup_html(const cJSON *product, const char *buy_action);
void strike_reward_render(StrikeScreen s, HtmlElem root);
int  strike_reward_action(const char *name);           // 1 = one of its strikeAction()s
void strike_reward_tick(Client *client);
void strike_reward_scenario(const cJSON *state);       // state.strike "rewardScreen" / "rewardChoice"
// Oo(itemDef): an item's power label (+bonus % and its stat icon) by catalogue id; 0 = it has none
int strike_item_power(const char *item_id, char *bonus, size_t n, const char **img);
// strike_menu.c: Fne(weapon): the Strike class whose primary weapon it is (its name), NULL none
const char *strike_menu_class_for_weapon(int weapon);
// REWARD's stars (oO: epic and better items): spin_wheel.c draws them into the screen's own canvas
struct SpinStarsDraw;
const struct SpinStarsDraw *strike_reward_stars(void);

// Module hooks called by strike_ui.c.
void strike_menu_install(void);                   // inline handlers of the menu screens
void strike_menu_render(StrikeScreen s, HtmlElem root); // after a screen's markup is inserted
void strike_menu_tick(Client *client);
void strike_menu_action(const char *name);         // strikeAction(name) (tests: pageActions / the e2e)
const char *strike_menu_template(StrikeScreen s);  // template file of a menu screen, NULL none
// SPIN's wheel this frame (visible 0 when SPIN is not up): spin_wheel.c draws it into the page's canvas
struct SpinWheelDraw;
const struct SpinWheelDraw *strike_menu_wheel_draw(void);
// Game events (client/src/net/game_events.c, beside hud_ui.c's): no-ops unless strike_ui_active().
void strike_hud_killfeed(Client *client, const char *killer, int killer_is_you, const char *victim, int victim_is_you,
                         const Weapon *weapon, int headshot, int thrown, int wallbang); // killer NULL: a suicide / world death
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
void strike_menu_scenario_board(const cJSON *rows); // state.strike.leaderboard (LEADERBOARD_PLAYERS' rows)
void strike_menu_scenario_state(const cJSON *st);   // state.strike.itemSelection (T5 over Customize), .product (U2)
