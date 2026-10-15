// In-game HUD over the DOM-ops API (client/include/html_dom.h): the C port of Krunker
// v7.1.4's HUD JavaScript (src/index.js io_* handlers, src/libs/UI.js toggleGameUI /
// toggleControlUI, src/data/players.js updatePlayerAmmo / weapon rack, src/ui/game.js
// GUI.endScreen); no JS runs natively.
//
// client_hud_html_tick() samples the game every frame (writes change-gated, fps every
// 100 ms) and writes v7's #inGameUI markup directly: one DOM write group per changed value
// group. Every write mirrors what the real v7.1.4 client does with default settings
// (DOM of its sandbox game, tools/uidiff capture):
//   UI.toggleGameUI/toggleControlUI -> which holders are shown (hud_apply_visibility)
//   updatePlayerAmmo / reloadUIAnim -> #ammoVal/#ammoMax, #ammoHolder background-size "N%  100%"
//   io_changeHealth: #bottomLeftHealth = ceil(hp), #bottomLeftHealthBar .nHealthBarSeg x segs
//   map load: #matchInfo = mode + "<div style='color:rgba(255,255,255,0.6)'>on " + map + "</div>"
//   KrunkNative has no minimap (v7 src/libs/Minimap.js): the top-left shows Minimap.toggle(false)'s
//     layout, #timerHolder/#timerInner/#timerIcon/#timerVal/#matchInfo with .topLeftOld
//   #pingText "-" while the ping is unknown (coloured by COLORS.ping), #fpsDisplay per showFPS
//   #weapHolder: one .weapItem per loadout slot (key name, weapon name, 0.5 opacity when holstered)
//   chat: map welMsg + "<name> joined the game" rows; v7 rows carry data-tab (chat channel)
//   f2024: <div id='feed_N'><div class='killfeedItem'><span class='killfeedMsg'>..</span></div><br>
//   f1985 (leaderType 1): #centerLeaderDisplay table while the scoreboard key is held
//   game-overlay default crosshair (type 1): four 4x21 bars on a square of
//     max(58, spread * 1.1 * uiHeight / 693), opacity = 1 - aimVal, hidden while reloading
//   game-overlay showHit(): four 7x25 bars at 45deg, spacing 37 (+0.14/ms), fading 0.0062/ms
// Krunker draws the last two on its <canvas id="game-overlay">; the page builds them from
// divs inside #inGameUI (ui/css/hud.css) so they scale with #uiBase like the canvas.
//
// Test hooks (`?kn_autoplay_spectate=1` / KRUNKNATIVE_AUTOPLAY_SPECTATE=1 makes the
// autoplay enter as a spectator): `?kn_autoplay=1` (native: KRUNKNATIVE_AUTOPLAY=1) spawns through
// client_enter_game() as soon as the page and the game are ready, so a headless
// browser can screenshot the HUD without clicking Play; `?kn_hud_demo=1`
// (KRUNKNATIVE_HUD_DEMO=1) adds kill-feed rows and hit markers on a loop.
#include <player_accounts.h>
#include <backend_ui.h> // account_get: the local player's clan in the player list
#include <client.h>

// Game-event entry points (prototypes belong in client/include/ui_logic.h):
//   hud_ui_add_killfeed: one kill-feed row; killer/victim are player names, weapon_icon
//     is the Weapon.icon stem ("icon_2") or NULL/"" for a text-only row.
//   hud_ui_hit: restarts the hit-marker animation (kill = red, 1.2x bars).
void hud_ui_add_killfeed(Client*, const char *killer, const char *victim, const char *weapon_icon, int is_headshot);
void hud_ui_hit(Client*, int kill);

#include <html_ui.h>
#include <html_dom.h>
#include <ui_logic.h>
#include <third_person.h>
#include <hud_map.h> // part 2a: minimap layout
#include <render_settings.h>
#include "../viewer/viewer_store.h" // store.skins: the melee skin icon
#include <streamer.h>
#include <spectate.h>
#include <ui_mobile.h>
#include <hud_gui.h> // map-G: the map's custom GUI (hud_gui.c)
#include <lobby_ui.h> // WP-E: the end screen's match vote cards (net/lobby_events.c)
#include <badges.h>   // WP-E: v7 badges on the boards
#include <progression.h> // WP-E part 2c: overlay.medalsList (the Medals tab)
#include <hud_social.h> // WP-E part 2b: mute lists, chat channels
#include <online_mode.h> // online_mode_server_rights: the player list's staff buttons
#include <net_ext.h>     // NET_RIGHTS_*
#include <voice_chat.h> // voice_is_speaking: the player list speaker icon
#include <menu_view.h>  // menu_view_time_scale: the round-end slow motion scales the title's dt
#include <input_script.h> // input_script_delta: KRUNKNATIVE_FIXED_DELTA frames
#include "vic_title.h" // overlay.vicTitle (GL/DOM-free, client/tests/vic_title_test.c)
#include <cJSON.h>
#include <limits.h>
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ruleset.h>
#include <hud_v10.h>
#include <nettspend.h> // KN-only weapon icon art (ui/img/weapons/)
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define HUD_UI_MIN_INTERVAL 0.0    // HUD sampled every frame like index.js gameLoop; writes stay change-gated
#define HUD_UI_FPS_INTERVAL 0.1    // seconds between #ingameFPS updates (lastFpsDisplayUpdate >= 100)
#define HUD_UI_LOADOUT_MAX 3       // Player.loadout slots mirrored into #weapDisplay
// KrunkNative Object Manager: with extra loadout weapons every slot shows (a compact rack)
#define HUD_UI_LOADOUT_ALL (NET_LOADOUT_MAX + 1 + PLAYER_EXTRA_WEAPONS_MAX)

// Krunker colors (13.js): killFeed, hudHealth, ping, chat server messages, xp bar.
#define FEED_COLOR_ENEMY "#eb5656"
#define FEED_COLOR_ALLY "#9eeb56"
#define FEED_COLOR_YOU "#fff"
#define HEALTH_COLOR_LOW "#eb5656"
#define HEALTH_COLOR_HIGH "#9eeb56"
#define PING_COLOR_LOW "rgb(158, 235, 86)"    // COLORS.ping (v3.1.1 src/data/colors.js)
#define PING_COLOR_MEDIUM "rgb(255, 213, 66)"
#define PING_COLOR_HIGH "rgb(255, 75, 66)"
#define SERVER_MSG_COLOR "#fc03ec"
#define SCORE_COLOR "#ffea78"      // settings.interface.scoreColor default (kill card +score)

#define FEED_LIMIT 8               // settings.editing.feedLimit default
#define FEED_MAX 48                // rows kept at most: feedLimit (<= 10) in #killFeed + chatListLength in #chatList
#define FEED_LIFE_MS 4000.0        // gE(): life = 4e3 in #killFeed, chatTimer * 1e3 in #chatList (0 = never)
#define FEED_DELTA_MAX_MS 33.0     // config.dltMx = round(1000 / 30): the clamped loop delta that ages bR rows
#define CHAT_LIST_LENGTH 35        // config.chatListLength
#define CHAT_BG_OPACITY_DEFAULT 0.2f // settings.chat.chatBGOp default: rows get no inline background then

#define UI_DESIGN_HEIGHT 693.0     // scaleBase: crosshairScale = viewportHeight / scale / 693
#define UI_DEFAULT_HEIGHT 1147.5   // #uiBase height before scaleBase ran
#define CROSS_MIN 58.0             // game-overlay crosshair type 1
#define CROSS_MLT 1.1
#define HIT_THICK 7.0              // settings.hitmarker defaults
#define HIT_LEN 25.0
#define HIT_SPACING 37.0
#define HIT_INC 0.14               // spacing px per ms
#define HIT_FADE 0.0062            // opacity per ms
#define HIT_KILL_MLT 1.2
#define HIT_COLOR "#ffffff"
#define HIT_KILL_COLOR "#ff0000"

#define LEADER_MAX 10              // updateLeaderboard (FFA): rows stop once the counter passes 10
#define HIT_LIFE_MS 2000.0         // config.hitLife: damage-direction indicator lifetime
#define DMG_HEIGHT 83.0            // game-overlay am(): indicator rect height (overlay px)
#define DMG_COLOR "255,63,63"      // game-overlay hitColor
#define DMG_MAX 8                  // indicator elements kept (Krunker reuses dead aV.hits entries)
#define DMG_NUM_MAX 16             // damage numbers kept (Krunker reuses dead dmgNums entries)
#define DMG_NUM_LIFE_MS 1000.0     // NJ2(): life = 1000
#define SPEED_UPDATE_MS 100.0      // speedometer value refresh (Qp9.update = 100)
#define HUD_SEED_WAIT_SAMPLES 40   // HUD samples (20 Hz) the chat seed waits for the server name
#define END_ANIM_MS 5000.0         // config.endAnim: title animation before the results (fu())
#define END_TITLE_W 768.0          // img/vic.png, def.png, dra.png
#define END_TITLE_H 512.0

#define LRM "\xE2\x80\x8E"         // U+200E, UTILS.fixStr wraps names so RTL text cannot reorder a row


// config.suicides (1.js): "<victim> committed <random entry>" kill-feed rows.
static const char *const SUICIDES[] = {"uninstall life", "toaster bath", "alt f4", "not alive", "neck rope", "scooter ankle", "death.exe"};

// Values sampled next to HtmlHudState (client.h): crosshair spread, aim fade,
// scoreboard key, weapon list.
typedef struct {
    int spread;                       // Krunker player.spread (crosshair size), rounded
    int aim_pct;                      // aim_val * 100 (crosshair fade), rounded
    int scoreboard;                   // Tab held while the game owns the input
    int loadout_index;
    int loadout_size;
    int loadout_base;                 // Object Manager: slots from here on are extra weapons (number keys)
    const Weapon *loadout[HUD_UI_LOADOUT_ALL];
    const ViewerSkin *skin[HUD_UI_LOADOUT_ALL]; // the skin worn on each slot (getSkinForLoadout / meleeIndex), NULL none
    int can_throw;                    // player.canThrow: melee slot shows the knife, else fists (thrown)
    int melee_icon;                   // store.skins[meleeIndex].id || 0: melee/icon_<id> of the melee skin
} HudUiExtra;

// One gE() row (Krunker's bR entry): #feed_N in #killFeed, or #chatMsg_N in #chatList
// with Old Scoreboard (leaderType 0). Removed outright when its life runs out (no fade).
typedef struct {
    HtmlElem row;
    double life_ms;          // > 0 counts down; 0 = lives until evicted (chatTimer 0)
    int chat;                // row lives in #chatList (Old Scoreboard)
} FeedRow;

// One overlay dmgNums entry (NJ2) and its DOM element.
typedef struct {
    HtmlElem el;
    double life_ms;          // > 0 while alive
    float x, y, z, dir;      // world position (drifts up and sideways), drift direction
    int value, crit;
    int shown;
} DamageNum;

// One aV.hits entry (damage-direction indicator) and its DOM element.
typedef struct {
    HtmlElem el;             // 0 until created
    double life_ms;          // > 0 while alive (config.hitLife countdown)
    float x, z;              // dealer position (Krunker "h" packet)
    int shown;               // last written display (block/none)
} DamageHit;

typedef struct {
    int visible;             // last applied visibility: -1 never applied
    int in_game;             // last applied #uiBase.onGame
    int in_game_was;         // client->in_game of the previous tick (session edge -> chat seed)
    int chat_seeded;         // join messages written for this session
    int chat_seq;            // next chatMsg_N
    int feed_seq;            // next feed_N
    HudUiExtra extra;        // last written extra values
    float ui_height;         // #uiBase layout height (crosshair scale)
    int cross_dirty;         // #uiBase resized: rewrite the crosshair
    int cross_shown;         // last written #knCrosshair display (-1 unknown)
    char cross_size[24];     // last written --kn-cross-size
    char cross_opacity[16];  // last written #knCrossCus opacity (custom layers)
    char cross_def_opacity[16]; // last written #knCrossDef opacity (default bars)
    char cross_sig[512];     // crosshair settings the #knCrosshair markup was built for
    char cross_opts[64];     // crosshairAlways/UseOpacity/Opacity last applied
    HtmlElem cross_def;      // #knCrossDef: default bars (crosshairSho 1/3)
    HtmlElem cross_cus;      // #knCrossCus: custom/image/precision layers (crosshairSho 2..5)
    HtmlElem crosshair;      // #knCrosshair (0 until created)
    HtmlElem hitmarker;      // #knHitmarker (0 until created)
    int hit_active;
    double hit_start_ms;
    double hit_opacity, hit_spacing, hit_inc, hit_fade; // hitSet values captured by hud_ui_hit
    FeedRow feed[FEED_MAX];
    int feed_count;
    double feed_last_ms;     // previous hud_feed_step clock (< 0: none yet)
    int autoplay_checked;
    int autoplay;
    int autoplay_done;
    int demo;                // kn_hud_demo: feed rows + hit marker every few seconds (test only)
    double demo_next;
    int ping;                // last written #pingText value (-1 = "-", INT_MIN = never written)
    int reloading;           // crosshair hidden while reloading (Krunker: aimVal * (reloading ? 0 : 1))
    int inspecting;          // crosshair hidden while inspecting (Krunker: * (inspecting ? 0 : 1))
    int chat_shown;          // last applied #chatUI display (-1 never applied)
    int dead;                // death card up (UI.toggleGameUI(false) applied, hud_ui_show_death)
    int ended;               // end screen up (UI.showEndScreen, hud_ui_show_end_screen)
    int board_shown;         // #newLeaderDisplay last written shown state (-1 unknown)
    int old_board;           // last applied oldScoreboard (-1 unknown): .topRightCounters width
    int rack_can_throw;      // canThrow the #weapDisplay melee icon shows (lastThrow)
    unsigned old_board_hash; // hash of the last #leaderContainer markup written (0 = none)
    unsigned board_hash;     // hash of the last #leaderContainerD markup written
    DamageHit dmg[DMG_MAX];
    double dmg_last_ms;      // HUD clock of the previous indicator step, < 0 = not stepping
    DamageNum nums[DMG_NUM_MAX];
    double nums_last_ms;     // previous damage-number step, < 0 = not stepping
    HtmlElem speed;          // #knSpeed (0 until created)
    int speed_shown;         // last written #knSpeed display
    int speed_cur, speed_max;// speedometer values (units / s)
    double speed_next_ms;    // next value refresh
    char speed_sig[160];     // speed settings the element was styled for
    int show_ui;             // showUI last applied to the visibility (-1 unknown)
    int seed_wait;           // samples the chat seed waited for the server name
    int end_phase;           // 0 none, 1 title animation (aV.vicTitle), 2 results (fu())
    double end_start_ms;     // HUD clock of the END
    HtmlElem end_title;      // #knVicTitle (0 until created)
    char end_title_key[64];  // last written title layer state
    VicTitle vic;            // overlay.vicTitle, stepped once per frame like overlay.render
    double tick_last_ms;     // HUD clock of the previous client_hud_html_tick (< 0 = none yet)
    double tick_jump_ms;     // scenario mode: the part of a pinned-clock jump still to replay
    int hide_default;        // KrunkScript game.ui.hidden (hud_ui_hide_default)
    int streak;              // last written #streakVal
} HudUi;

static unsigned g_combat_hash; // hud_combat_step: FNV-1a of the last written perks / streaks state, 0 = rewrite

static HudUi g_hud_ui = {.visible = -1, .in_game = -1, .cross_shown = -1, .ping = INT_MIN, .chat_shown = -1, .board_shown = -1, .old_board = -1, .feed_last_ms = -1.0, .dmg_last_ms = -1.0, .nums_last_ms = -1.0, .speed_shown = -1, .show_ui = -1,
                         .vic = {0.0, 0.05, 2.5, 0.005, 0.01}, .tick_last_ms = -1.0};

// Scenario values (client_bin --render-scenario, ui_scenario.c) that replace what the
// tick samples from the game; both NULL outside scenario mode.
typedef struct {
    cJSON *hud;         // owned copy of the scenario `state.hud` object
    cJSON *scoreboard;  // owned copy of `state.scoreboard` rows [{rank, name, score, me?}]
} HudUiOverride;

static HudUiOverride g_hud_override;
static double g_hud_clock_s = -1.0; // hud_ui_set_clock(): < 0 follows the caller's (GLFW) clock

/* ------------------------------------------------------------ strings */

// Bounded printf-append: keeps `*len` and never overflows `size`.
static void hb_append(char *buf, const size_t size, size_t *len, const char *fmt, ...) {
    if (*len >= size) return;

    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(buf + *len, size - *len, fmt, args);
    va_end(args);

    if (n < 0) return;
    *len = (size_t) n >= size - *len ? size - 1 : *len + (size_t) n;
}

// HTML-escape &<>"' (hud.js esc()).
static void hb_append_escaped(char *buf, const size_t size, size_t *len, const char *text) {
    for (const char *p = text ? text : ""; *p; p++) {
        switch (*p) {
            case '&': hb_append(buf, size, len, "&amp;"); break;
            case '<': hb_append(buf, size, len, "&lt;"); break;
            case '>': hb_append(buf, size, len, "&gt;"); break;
            case '"': hb_append(buf, size, len, "&quot;"); break;
            case '\'': hb_append(buf, size, len, "&#39;"); break;
            default: hb_append(buf, size, len, "%c", *p); break;
        }
    }
}

// UTILS.fixStr: escaped and wrapped in U+200E marks.
static void hb_append_fixed(char *buf, const size_t size, size_t *len, const char *text) {
    hb_append(buf, size, len, LRM);
    hb_append_escaped(buf, size, len, text);
    hb_append(buf, size, len, LRM);
}

// JS Number -> string for the values the HUD prints: integers without a fraction,
// otherwise the shortest round-trip-ish form (%.16g matches JS for 1/3-style values).
static void fmt_number(const double value, char *out, const size_t size) {
    if (value == floor(value) && fabs(value) < 1e15) snprintf(out, size, "%.0f", value);
    else snprintf(out, size, "%.16g", value);
}

// String(+x.toFixed(3)): three decimals, trailing zeros and dot dropped ("1", "0.25").
static void fmt_opacity(const double value, char *out, const size_t size) {
    snprintf(out, size, "%.3f", value);

    char *dot = strchr(out, '.');
    if (!dot) return;

    char *end = out + strlen(out);
    while (end > dot && end[-1] == '0') *--end = 0;
    if (end == dot + 1) *dot = 0;
}

/* ------------------------------------------------------------ markup (pure) */

// utils.getTime: "MM:SS" of a whole number of seconds.
static void hud_timer_text(const int total_seconds, char *out, const size_t size) {
    const int total = total_seconds > 0 ? total_seconds : 0;
    snprintf(out, size, "%02d:%02d", total / 60, total % 60);
}

// io_changeHealth: #bottomLeftHealthBar.innerHTML = `segs` .nHealthBarSeg, each filled by the
// segment's share of hp/max, hudHealthLow once at most 3 segments worth of health are left
// (the colour also goes into `color`, which the CSS uses for the glow).
static void hud_health_bar_html(int hp, int max, int segs, char *out, const size_t size) {
    hp = hp < 0 ? 0 : hp;
    max = max < 1 ? (hp > 0 ? hp : 1) : max;
    segs = segs < 1 ? 1 : segs;

    const ClientRenderSettings *rs = client_render_settings();
    const double left = (double) segs * ((double) hp / (double) max); // segments worth of health left
    const char *color = left <= 3.0 ? (rs->hud_health_low[0] ? rs->hud_health_low : HEALTH_COLOR_LOW)
                                    : (rs->hud_health_high[0] ? rs->hud_health_high : HEALTH_COLOR_HIGH);
    size_t len = 0;
    out[0] = 0;

    for (int i = 0; i < segs; i++) {
        const double v = left - (double) i;
        char width[32];
        fmt_number((v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v) * 100.0, width, sizeof(width));
        hb_append(out, size, &len, "<div class='nHealthBarSeg'><div class='nHpBSeg' style='width:%s%%;background-color:%s;color:%s'></div></div>",
                  width, color, color);
    }
}

// .weapIcon: `canThrow && melee` -> the melee skin icon (default knife: melee/icon_0),
// else weaponConfig.icon; a thrown knife (canThrow false) leaves the melee slot on the
// Combat Knife's own icon_0, the fists, until refillKnife / respawn.
static const char *hud_weapon_icon_src(const Weapon *slot, const int can_throw, const int melee_icon, char *out, const size_t size) {
    if (slot && slot->melee && can_throw && melee_icon > 0) {
        char path[64]; // a skin's icon lives with the game assets, not in the page's textures/
        snprintf(path, sizeof(path), "textures/melee/icon_%d.png", melee_icon);
        html_dom_asset_url(path, out, size);
    } else if (slot && slot->melee && can_throw) snprintf(out, size, "textures/melee/icon_0.png");
    else if (!nettspend_weapon_icon_src(slot ? slot->icon : NULL, out, size)) // KN-only art (nettspend.h): ui/img/weapons/
        snprintf(out, size, "textures/weapons/%s.png", slot && slot->icon ? slot->icon : "icon_0");
    return out;
}

// weaponConfig index of `w` (the default table; maps / custom games reuse its ids), 0 unknown.
static int weapon_index_of(const Weapon *w) {
    for (int i = 0; i < WEAPON_TABLE_COUNT; i++) { // KrunkNative-only weapons too (the Piss)
        if (rules_weapon(i) == w) return i;
    }
    return 0;
}

// The key name printed on slot `i` > 0 (v7 players.js swapWeapon): the melee bind for a melee
// weapon, the equipment bind for equipment, else the swap bind (UTILS.getKeyName).
static const char *hud_weapon_key(const Weapon *w, char *out, const size_t size) {
    const int id = w && w->melee ? UI_CONTROL_MELEE : w && w->equipment ? UI_CONTROL_EQUIP : UI_CONTROL_SWAP;
    return ui_controls_key_name(ui_controls_bind(id), out, size);
}

// #weapHolder.innerHTML (players.js swapWeapon, isYou): one .weapItem per slot, the held one
// at opacity 1, holstered ones at 0.5; inside .weapItemInner the key name (slots > 0), the
// label (the worn skin's name in its rarity colour, else the weapon name in #fff), the per-weapon
// kill streak (hidden at 0) and the icon. Only #weapItem_N's opacity changes on a swap.
// v10's label span: class weapSkin (+ whiteShad on rarity 5), style color = rarities[rarity || 0].color,
// text = skin.name || weapon.name. No rainbow class and no nametag here: the nametag branch
// (a custom skin entry with a name in its metadata) has no KrunkNative counterpart.
static void hud_weapon_rack_html(const Weapon *const *loadout, const ViewerSkin *const *skins, const int count, const int base, const int index,
                                 const int can_throw, const int melee_icon, char *out, const size_t size) {
    size_t len = 0;
    out[0] = 0;

    for (int i = 0; i < count && i < HUD_UI_LOADOUT_ALL; i++) {
        const Weapon *w = loadout[i];
        const ViewerSkin *skin = skins[i];
        char src[256], key[32];
        hb_append(out, size, &len, "<div class='weapItem' id='weapItem_%d' style='opacity:%s'><div class=\"weapItemInner\">", i, i == index ? "1" : "0.5");
        if (i >= base && i < 9) { // Object Manager extra slot: its number key (ui_controls.c)
            hb_append(out, size, &len, "<div class='weapKey'>%d</div>", i + 1);
        } else if (i && i < base) {
            hb_append(out, size, &len, "<div class='weapKey'>");
            hb_append_escaped(out, size, &len, hud_weapon_key(w, key, sizeof(key)));
            hb_append(out, size, &len, "</div>");
        }
        if (skin && skin->name) {
            const int rarity = skin->rarity >= 0 && skin->rarity < g_viewer_rarity_count ? skin->rarity : 0;
            const char *color = g_viewer_rarities[rarity].color ? g_viewer_rarities[rarity].color : "#fff";
            hb_append(out, size, &len, "<span class='weapSkin%s' style='color:%s'>", skin->rarity == 5 ? " whiteShad" : "", color);
            hb_append_escaped(out, size, &len, skin->name);
        } else {
            hb_append(out, size, &len, "<span class='weapSkin' style='color:#fff'>");
            hb_append_escaped(out, size, &len, w && w->name ? w->name : "");
        }
        hb_append(out, size, &len, "</span><div class='weapStreakHolder' data-weapon-index='%d' style='display:none'><div class='weapStreakInner'>"
                                   "<span class='weapStreak'>0</span><span style='color: var(--yellow)'>KS</span></div></div>",
                  w ? weapon_index_of(w) : 0);
        hb_append(out, size, &len, "<img class='weapIcon' src='%s' %s/></div></div>",
            hud_weapon_icon_src(w, can_throw, melee_icon, src, sizeof(src)), w && w->melee ? "id='weapIconMel' " : "");
    }
}

// curGameInfo.innerHTML
static void hud_game_info_html(const char *mode, const char *map, char *out, const size_t size) {
    size_t len = 0;
    out[0] = 0;
    hb_append_escaped(out, size, &len, mode);
    hb_append(out, size, &len, "<div style='color:rgba(255,255,255,0.6)'>on ");
    hb_append_escaped(out, size, &len, map);
    hb_append(out, size, &len, "</div>");
}

// io_addChat -> f2018: the inner markup of a #chatMsg_N row. A `name` makes a player line
// (fixStr(name) + ": " + message), else a `color` makes a server line (type 2), else a
// plain game line (type 1, .chatMsg default color). chatTextOutline / chatBGOp style the
// .chatItem like v7's (v12018 / v12017).
static void hud_chat_row_html(const char *name, const char *msg, const char *color, char *out, const size_t size) {
    const ClientRenderSettings *rs = client_render_settings();
    size_t len = 0;
    out[0] = 0;
    hb_append(out, size, &len, "<div class='chatItem%s'", rs->chat_text_outline ? " chatTextOutline" : "");
    if (fabsf(rs->chat_bg_opacity - CHAT_BG_OPACITY_DEFAULT) > 1e-4f)
        hb_append(out, size, &len, " style='background-color: rgba(0, 0, 0, %g)'", (double) rs->chat_bg_opacity);
    hb_append(out, size, &len, ">");
    if (name && name[0]) {
        char shown[PLAYER_NAME_SIZE];
        hb_append_fixed(out, size, &len, streamer_name(name, shown, sizeof(shown))); // addChat: scrambleS(sender) in streamer mode
        hb_append(out, size, &len, ": <span class='chatMsg'>");
    } else if (color && color[0]) {
        hb_append(out, size, &len, "<span class='chatMsg' style='color:");
        hb_append_escaped(out, size, &len, color);
        hb_append(out, size, &len, "'>");
    } else {
        hb_append(out, size, &len, "<span class='chatMsg'>");
    }
    hb_append_fixed(out, size, &len, msg);
    hb_append(out, size, &len, "</span></div><br>");
}

// aU.formatNum(n, 2): "" + n below 1000, else n / 1000^p with two decimals + k/m/b.
static void hud_format_num(const int value, char *out, const size_t size) {
    const double v = value;
    const int p = value ? (int) floor(log(fabs(v)) / log(1000.0)) : 0;
    if (p >= 1 && p <= 3) snprintf(out, size, "%.2f%c", v / pow(1000.0, p), "kmb"[p - 1]);
    else snprintf(out, size, "%d", value);
}

// The board columns: the mode's leaderStats (v7 gameModes/*.js, default ["score"]; FFA/TDM
// ["score", "kills", "deaths"], Zombies ["points", "zombies", "downs", "dmg"]), headed
// UTILS.capFirst(key). The values come from NET_PACKET_LEADER_STATS (v7 "7"), else from the
// local Player fields (mode_leader_stat_value).
#define BOARD_STAT_MAX NET_LEADER_STATS_MAX
static const char *const BOARD_DEFAULT_STATS[] = {"score", NULL};

static const char *const *hud_board_keys(const Client *client, int *count) {
    const char *const *keys = client && client->game.mode && client->game.mode->config.leader_stats ? client->game.mode->config.leader_stats
                                                                                                    : BOARD_DEFAULT_STATS;
    int n = 0;
    while (keys[n] && n < BOARD_STAT_MAX) n++;
    *count = n;
    return keys;
}

// The last NET_PACKET_LEADER_STATS (hud_ui_set_leader_stats): board order + column values.
static NetLeaderStatsPacket g_leader_stats;
static int g_leader_stats_valid;

void hud_ui_set_leader_stats(const NetLeaderStatsPacket *packet) {
    if (!packet) {
        g_leader_stats_valid = 0;
        return;
    }
    g_leader_stats = *packet;
    if (g_leader_stats.count > GAME_LEADERBOARD_MAX) g_leader_stats.count = GAME_LEADERBOARD_MAX;
    if (g_leader_stats.stat_count > NET_LEADER_STATS_MAX) g_leader_stats.stat_count = NET_LEADER_STATS_MAX;
    g_leader_stats_valid = 1;
}

// The " [clan]" tag of a board row: COLORS.getClanCol(clan, fallback) (v7.1.4: DEV, VIP and CMD
// keep a colour; the clan war rank colours are gone).
static void hb_append_clan(char *out, const size_t size, size_t *len, const char *clan, const char *fallback) {
    if (!clan || !clan[0]) return;
    const char *col = !strcmp(clan, "DEV") ? "#40C4FF" : !strcmp(clan, "VIP") ? "#11D445" : !strcmp(clan, "CMD") ? "#993299" : fallback;
    hb_append(out, size, len, "<span style='color:%s'> [", col);
    hb_append_escaped(out, size, len, clan);
    hb_append(out, size, len, "]</span>");
}

// premiumT "beenhere" / featured "check_circle" icons and the custom badge image of a board row
// (acc: the player's account from NET_PACKET_PLAYER_INFO, NULL for guests; badge: badgeIndex,
// -1 none); `top` is the icons' margin-top (f1984 5 px, f1985 3 px). The partner "verified"
// icon needs partnerApp, which PLAYER_INFO does not carry.
static void hb_append_badges(char *out, const size_t size, size_t *len, const NetPlayerInfoPacket *acc, const int badge, const int top) {
    if (acc && acc->account_id > 0 && (acc->flags & NET_PLAYER_INFO_PREMIUM))
        hb_append(out, size, len, " <i class='material-icons' style='color:#FBC02D;margin-top:%dpx;font-size:23px;vertical-align:middle;'>beenhere</i>", top);
    if (acc && acc->account_id > 0 && (acc->flags & NET_PLAYER_INFO_VERIFIED))
        hb_append(out, size, len, " <i class='material-icons' style='color:#40C4FF;margin-top:%dpx;font-size:23px;vertical-align:middle;'>check_circle</i>", top);
    char tex[64], url[160];
    if (!badge_get(badge)) return;
    html_dom_asset_url(badge_texture(badge, tex, sizeof(tex)), url, sizeof(url)); // Game.badges[i].url (asset host)
    hb_append(out, size, len, "<img src='%s' style='margin-top:%dpx;font-size:28px;vertical-align:middle;height:21px;margin-left:2px;'>", url, top);
}

// One board entry, sampled from the game or a scenario row.
typedef struct {
    const char *name;
    int stats[BOARD_STAT_MAX];    // one per hud_board_keys() column
    int is_you, is_ally, team;
    int badge;                    // badgeIndex, -1 none
    const NetPlayerInfoPacket *acc;
} BoardRow;

// f1984 (Old Scoreboard, #leaderContainer): one .leaderItem per player, at most LEADER_MAX.
static void hud_old_board_row_html(char *out, const size_t size, size_t *len, const int rank, const BoardRow *r) {
    char num[32], shown[PLAYER_NAME_SIZE];
    hud_format_num(r->stats[0], num, sizeof(num));
    hb_append(out, size, len, "<div class='leaderItem'><div class='leaderCounter'>%d.</div>", rank);
    hb_append_badges(out, size, len, r->acc, r->badge, 5);
    hb_append(out, size, len, "<div class='leaderName%s'>", r->is_you ? "M" : r->is_ally ? "F" : "");
    hb_append_escaped(out, size, len, streamer_name(r->name && r->name[0] ? r->name : "Guest", shown, sizeof(shown))); // streamMode ? scrambleS(alias)
    hb_append_clan(out, size, len, r->acc && r->acc->account_id > 0 ? r->acc->clan : NULL, "#fff");
    hb_append(out, size, len, "</div><div class='leaderScore'>%s</div></div>", num);
}

// f1985's row of the #centerLeaderDisplay table.
static void hud_center_board_row_html(char *out, const size_t size, size_t *len, const int rank, const BoardRow *r, const int columns) {
    char shown[PLAYER_NAME_SIZE];
    hb_append(out, size, len, "<tr><td><div class='newLeaderItem'><div class='newLeaderCounter'>%d.</div>", rank);
    hb_append_badges(out, size, len, r->acc, r->badge, 3);
    hb_append(out, size, len, "<div class='newLeaderName%s'>", r->is_you ? "M" : r->is_ally ? "F" : "");
    hb_append_escaped(out, size, len, streamer_name(r->name && r->name[0] ? r->name : "Guest", shown, sizeof(shown)));
    hb_append_clan(out, size, len, r->acc && r->acc->account_id > 0 ? r->acc->clan : NULL, "#fff");
    hb_append(out, size, len, "</div></div></td>");
    for (int i = 0; i < columns; i++) {
        char num[32];
        hud_format_num(r->stats[i], num, sizeof(num));
        hb_append(out, size, len, "<td>%s</td>", num);
    }
    hb_append(out, size, len, "</tr>");
}

static const char *hud_player_name(const Client *client, const Player *player) {
    if (player->name[0]) return player->name;
    return player == client->me ? client->menu_name : "Guest";
}

// The local player's name as the server knows it (SPAWN "Guest_N"), else the menu name.
static const char *hud_my_name(const Client *client) {
    if (!client) return "";
    return client->me ? hud_player_name(client, client->me) : client->menu_name;
}

// The server's player list (client->game.players, LEADERBOARD packets) sorted by score,
// highest first; equal scores keep the list order (Krunker sorts server side).
static int hud_sorted_players(const Client *client, const Player **out, const int max) {
    int n = 0;
    for (size_t i = 0; i < client->game.player_count && n < max; i++) {
        const Player *player = client->game.players[i];
        if (!player) continue;
        int at = n++;
        while (at > 0 && out[at - 1]->score < player->score) {
            out[at] = out[at - 1];
            at--;
        }
        out[at] = player;
    }
    return n;
}

#define HUD_BOARD_PLAYERS_MAX 128

static int hud_team_mode(const Client *client) {
    return client->game.mode && client->game.mode->config.teams && !client->game.mode->config.fake_names;
}

// leaderName "M" (you), else "F" when Game.mode.fakeNames or a team mate, else plain.
static int hud_board_ally(const Client *client, const int is_you, const int team, const int my_team) {
    if (is_you) return 0;
    if (client->game.mode && client->game.mode->config.fake_names) return 1;
    return team && team == my_team;
}

// One live row: the player's name, team, account, badge and the column values (the server's
// NET_PACKET_LEADER_STATS values when it sent some for this player, else the local fields).
static void hud_board_fill(const Client *client, const Player *p, const int32_t *values, const int value_count, BoardRow *r) {
    int columns;
    const char *const *keys = hud_board_keys(client, &columns);
    const int my_team = client->me ? client->me->team : 0;
    memset(r, 0, sizeof(*r));
    r->name = hud_player_name(client, p);
    for (int k = 0; k < columns; k++) r->stats[k] = values && k < value_count ? values[k] : mode_leader_stat_value(p, keys[k]);
    r->team = p->team;
    r->is_you = p == client->me;
    r->is_ally = hud_board_ally(client, r->is_you, r->team, my_team);
    r->acc = player_accounts_get(p->uid);
    r->badge = p->cosmetics_v7.badge;
}

// The board rows: scenario rows [{rank?, name, score, kills?, deaths?, <leaderStat>?, me?, team?}]
// when a scenario supplies them, else the server's board order (NET_PACKET_LEADER_STATS: the
// mode's leaderSort, at most 16 rows), else every known player sorted by score, else just the
// local player.
static int hud_board_rows(const Client *client, const cJSON *rows, BoardRow *out, const int max) {
    int n = 0, columns;
    const char *const *keys = hud_board_keys(client, &columns);
    const char *my_name = hud_my_name(client);
    const int my_team = client->me ? client->me->team : 0;
    if (cJSON_IsArray(rows)) {
        const int count = cJSON_GetArraySize(rows);
        const cJSON *row;
        cJSON_ArrayForEach(row, rows) {
            if (n >= max) break;
            const cJSON *name = cJSON_GetObjectItemCaseSensitive(row, "name");
            const cJSON *me = cJSON_GetObjectItemCaseSensitive(row, "me");
            BoardRow *r = &out[n++];
            memset(r, 0, sizeof(*r));
            r->name = cJSON_IsString(name) ? name->valuestring : "";
            // an FFA board always lists the local player: a single row is them
            r->is_you = cJSON_IsBool(me) ? cJSON_IsTrue(me) : count == 1 || (my_name[0] && !strcmp(r->name, my_name));
#define ROW_INT(key) (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(row, key)) ? cJSON_GetObjectItemCaseSensitive(row, key)->valueint : 0)
            for (int k = 0; k < columns; k++) r->stats[k] = ROW_INT(keys[k]);
            r->team = ROW_INT("team");
            r->badge = cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(row, "badge")) ? ROW_INT("badge") : -1;
#undef ROW_INT
            r->is_ally = hud_board_ally(client, r->is_you, r->team, my_team);
        }
        return n;
    }
    if (g_leader_stats_valid && client->game.players) {
        for (int i = 0; i < g_leader_stats.count && n < max; i++) {
            const Player *p = game_find_player((Game *) &client->game, g_leader_stats.uid[i]);
            if (p) hud_board_fill(client, p, g_leader_stats.values[i], g_leader_stats.stat_count, &out[n++]); // findBySid: gone players drop out
        }
        if (n) return n;
    }
    const Player *players[HUD_BOARD_PLAYERS_MAX];
    const int count = client->game.players ? hud_sorted_players(client, players, HUD_BOARD_PLAYERS_MAX) : 0;
    for (int i = 0; i < count && n < max; i++) hud_board_fill(client, players[i], NULL, 0, &out[n++]);
    if (n == 0 && max > 0) {
        BoardRow *r = &out[n++];
        memset(r, 0, sizeof(*r));
        r->name = my_name;
        for (int k = 0; k < columns; k++) r->stats[k] = client->me ? mode_leader_stat_value(client->me, keys[k]) : 0;
        r->team = my_team;
        r->is_you = 1;
        r->badge = client->me ? client->me->cosmetics_v7.badge : -1;
    }
    return n;
}

// f1984: the Old Scoreboard's #leaderContainer markup ("" = "Empty Lobby").
static void hud_old_board_html(const Client *client, const cJSON *rows, char *out, const size_t size) {
    BoardRow board[HUD_BOARD_PLAYERS_MAX];
    const int n = hud_board_rows(client, rows, board, HUD_BOARD_PLAYERS_MAX);
    size_t len = 0;
    out[0] = 0;
    for (int i = 0; i < n && i < LEADER_MAX; i++) hud_old_board_row_html(out, size, &len, i + 1, &board[i]);
}

// f1985: the #centerLeaderDisplay markup. Team modes (mode.teams && !fakeNames) get one
// .centerLeaderContainer per team with its .newLeaderTeam header (team name + score).
static void hud_scoreboard_html(const Client *client, const cJSON *rows, char *out, const size_t size) {
    BoardRow board[HUD_BOARD_PLAYERS_MAX];
    const int n = hud_board_rows(client, rows, board, HUD_BOARD_PLAYERS_MAX);
    const int teams = hud_team_mode(client);
    int columns;
    const char *const *keys = hud_board_keys(client, &columns);
    size_t len = 0;
    out[0] = 0;
    // v10 (v10:220499): the header rows in <thead>, the players in <tbody>
    for (int group = 0; group < (teams ? 2 : 1); group++) {
        int rank = 1;
        for (int i = 0; i < n; i++) {
            if (teams && board[i].team - 1 != group) continue;
            if (rank == 1) {
                hb_append(out, size, &len, "<div class=\"centerLeaderContainer\"><div class=\"centerLeaderContainerInner\">");
                if (teams) {
                    const GameModeConfig *mc = &client->game.mode->config;
                    const char *team_name = mc->team_options[group] ? mc->team_options[group] : group ? "Bravo" : "Alpha"; // #teamNm1/#teamNm2
                    hb_append(out, size, &len, "<div class='newLeaderTeam'>");
                    hb_append_escaped(out, size, &len, team_name);
                    hb_append(out, size, &len, "<div class='newLeaderTeamScore'>%d</div></div>", client->game.mode->team_scores[group + 1]);
                }
                hb_append(out, size, &len, "<table id='ingameTable'><thead><tr><th>Name</th>");
                for (int k = 0; k < columns; k++) // UTILS.capFirst(key)
                    hb_append(out, size, &len, "<th>%c%s</th>", toupper((unsigned char) keys[k][0]), keys[k] + 1);
                hb_append(out, size, &len, "</tr><tr height='10px;'></tr></thead><tbody>");
            }
            hud_center_board_row_html(out, size, &len, rank++, &board[i], columns);
        }
        if (rank > 1) hb_append(out, size, &len, "</tbody></table></div></div>");
    }
}

// windows[22] "Player List" (showWindow(23), opened by pListKey; v10.0.2 genList, game.deob.js
// :214284-214310): the header with the player count and the search box, then .pListTable rows:
// the local player first, then every player in Game.players.list (config.isProd hides the local
// player there). A row: the ping icon, the profile link (openPlayerProfile; right click copyUser),
// the speaker icon, then the actions v10 shows: chat / voice mute (players with an account,
// level >= 1), the K.P.D call (this account at level >= 5), Votekick (an account, not ranked, not a
// custom lobby, one vote at a time: hud_votekick.c) and the staff buttons (Kill, TP2, TPME, the
// punish popup) for DEV / the host, which run the server's own chat commands (/kill, /tp, /bring,
// /kick, /ban). A K.P.D officer's case (hud_votekick.c kpdData) puts the suspect first in a red
// row. Not ported: Trade (mode.canTrade: KrunkNative has no in-lobby trading), FREEZE, Kick Spec and
// the bots popup (no server action behind them yet).
static char g_plist_search[64]; // windows[22].playerSearch

typedef struct {
    int uid;
    char name[PLAYER_NAME_SIZE * 2];
    char clan[16];
    int ping;
    int account_id;  // 0 guest (v10 level 0: no mute buttons)
    int level;
    int hacker;      // isHacker: the red name
    int host;        // game.host == id: the star
    int speaking;    // voiceChatQueues[alias]
    int chat_muted, voice_muted; // scenario only (live rows ask hud_social)
} PlistRow;

static int hud_plist_matches(const char *name) {
    if (!g_plist_search[0]) return 1;
    char a[PLAYER_NAME_SIZE * 2], q[sizeof(g_plist_search)];
    size_t i;
    for (i = 0; name[i] && i + 1 < sizeof(a); i++) a[i] = (char) tolower((unsigned char) name[i]);
    a[i] = 0;
    for (i = 0; g_plist_search[i] && i + 1 < sizeof(q); i++) q[i] = (char) tolower((unsigned char) g_plist_search[i]);
    q[i] = 0;
    return strstr(a, q) != NULL;
}

static void hud_plist_clan(StrBuf *sb, const char *clan) {
    if (!clan || !clan[0]) return;
    // v10 COLORS.getClanCol(clan, "#353535") (game.deob.js:4306488): the staff clans are #FBC02D, any
    // other clan (VIP, CMD, ...) the #353535 default. The list is lowercased in v10.
    static const char *const staff[] = {"arae", "lain", "scte", "fame", "kpd", "jump", "art", "dev", "mmok", "nl"};
    int gold = 0;
    char low[32];
    size_t i;
    for (i = 0; clan[i] && i + 1 < sizeof(low); i++) low[i] = (char) tolower((unsigned char) clan[i]);
    low[i] = 0;
    for (size_t k = 0; k < sizeof(staff) / sizeof(staff[0]); k++) gold |= !strcmp(low, staff[k]);
    const char *col = gold ? "#FBC02D" : "#353535";
    sb_appendf(sb, "<span style='color:%s'> [", col);
    sb_append_esc(sb, clan);
    sb_append(sb, "]</span>");
}

// The staff rights v10 checks: account.isDev / canTeleport / canGlobalKick (the signed-in account:
// KrunkNative's DEV / moderator flags) and isHost (the server says this socket hosts the lobby:
// NET_EXT_KN_ACCOUNT_RIGHTS HOST). Running the offline sandbox is neither (v10's own sandbox shows
// no staff buttons and no host star either); the server checks every command again anyway.
static int hud_plist_staff(const Client *client, int *dev, int *host) {
    const AccountInfo *acc = account_get();
    *dev = acc && (acc->is_dev || acc->is_mod);
    *host = client && !client->offline && (online_mode_server_rights() & NET_RIGHTS_HOST) != 0;
    return *dev || *host;
}

static void hud_plist_focus(StrBuf *sb, const PlistRow *r) {
    if (!spectate_active()) return;
    sb_appendf(sb, "<span onmouseenter='playTick()' title='Focus Player' class='punishButton' onclick='focusPlayer(\"%d\", this)'>"
                   "<span class='takeAction material-icons'>%s</span></span>", r->uid, spectate_focused_uid() == r->uid ? "visibility_off" : "visibility");
}

static const char *hud_plist_ping_color(const int ping) {
    return ping >= 150 ? PING_COLOR_HIGH : ping >= 50 ? PING_COLOR_MEDIUM : ping <= 0 ? "#fff" : PING_COLOR_LOW; // COLORS.ping
}

// v10 genList's first row for a K.P.D officer: the suspect (kpdData.sus) in a red frame, the name red
static void hud_plist_suspect_row(StrBuf *sb, const Client *client, const PlistRow *r) {
    int dev = 0, host = 0;
    hud_plist_staff(client, &dev, &host);
    sb_append(sb, "<tr style='border:solid 2px red'><td class='pListName'>");
    hud_plist_focus(sb, r);
    sb_appendf(sb, "<span class='pListPing material-icons' style='color:%s' title='%dms'>signal_cellular_alt</span>"
                   "<a href='javascript:void(0)' class='red' onclick='openPlayerProfile(\"", hud_plist_ping_color(r->ping), r->ping > 0 ? r->ping : 0);
    sb_append_esc(sb, r->name);
    sb_appendf(sb, "\")' oncontextmenu='copyUser(this, &quot;%d&quot;)'>", r->uid);
    sb_append_esc(sb, r->name);
    hud_plist_clan(sb, r->clan);
    sb_append(sb, "</a></td></td><td class='pListActions'>"); // v10's own markup (the stray </td> included)
    if (dev || host)
        sb_appendf(sb, "<span onmouseenter='playTick()' class='punishButton kick' onclick='punishPopup(\"%d\")'>"
                       "<span class='takeAction material-icons'>construction</span></span>", r->uid);
    sb_append(sb, "</td></tr>");
}

static void hud_plist_row(StrBuf *sb, const Client *client, const PlistRow *r, const int scenario) {
    int dev = 0, host = 0;
    hud_plist_staff(client, &dev, &host);
    sb_append(sb, "<tr><td class='pListName'>");
    hud_plist_focus(sb, r);
    if (r->host) sb_append(sb, "<i class='material-icons' style='font-size:25px'>stars</i>"); // iíïïiìï == id: the lobby's host
    sb_appendf(sb, "<span class='pListPing material-icons' style='color:%s' title='%dms'>signal_cellular_alt</span>"
                   "<a href='javascript:void(0)' class='%s' onclick='openPlayerProfile(\"", hud_plist_ping_color(r->ping), r->ping > 0 ? r->ping : 0,
               r->hacker ? "red" : "");
    sb_append_esc(sb, r->name);
    sb_appendf(sb, "\")' oncontextmenu='copyUser(this, &quot;%d&quot;)'>", r->uid);
    sb_append_esc(sb, r->name);
    hud_plist_clan(sb, r->clan);
    sb_append(sb, "</a>");
    if (r->speaking) sb_append(sb, "<span class='pListVoice material-icons'>volume_up</span>");
    sb_append(sb, "</td><td class='pListActions'>");
    if (r->account_id > 0 || r->level >= 1) { // level >= 1: the buttons show mutedChat / VoiceAccountIds
        const int chat_muted = scenario ? r->chat_muted : social_chat_muted(r->account_id);
        const int voice_muted = scenario ? r->voice_muted : social_voice_muted(r->account_id);
        sb_appendf(sb, "<span title='Chat mute/unmute' onmouseenter='playTick()' class='muteUserBtn material-icons' onclick='muteUser(this, `%d`, 0)'>%s</span>",
                   r->uid, chat_muted ? "speaker_notes_off" : "chat");
        sb_appendf(sb, "<span title='Voice mute/unmute' onmouseenter='playTick()' class='muteUserBtn material-icons' onclick='muteUser(this, `%d`, 1)'>%s</span>",
                   r->uid, voice_muted ? "voice_over_off" : "record_voice_over");
    }
    if (hud_votekick_kpd_row_allowed(client)) {
        sb_append(sb, "<span onmouseenter='playTick()' class='punishButton kdf' onclick='confirmPolice(`");
        sb_append_esc(sb, r->name);
        sb_appendf(sb, "`,`%d`)'><img src='./img/police.png' style='height:25px;vertical-align:middle;margin-top:-6px'></span>", r->uid);
    }
    if (hud_votekick_row_allowed(client, r->name, 1))
        sb_appendf(sb, "<span onmouseenter='playTick()' class='punishButton vote' onclick='kickVote(\"%d\")'>Votekick</span>", r->uid);
    if (dev) sb_appendf(sb, "<span onmouseenter='playTick()' class='punishButton kill' onclick='punishPlayer(\"%d\", 3)'>Kill</span>", r->uid);
    if (dev) { // account.canTeleport: staff (the host teleports with /tp and /bring)
        sb_appendf(sb, "<span onmouseenter='playTick()' class='punishButton tp2' onclick='punishPlayer(\"%d\", 4)'>TP2</span>", r->uid);
        sb_appendf(sb, "<span onmouseenter='playTick()' class='punishButton tpme' onclick='punishPlayer(\"%d\", 5)'>TPME</span>", r->uid);
    }
    if (dev || host) // isHost || account.canGlobalKick / canSilentKick / canGlobalBan / canGlobalFlag
        sb_appendf(sb, "<span onmouseenter='playTick()' class='punishButton kick' onclick='punishPopup(\"%d\")'>"
                       "<span class='takeAction material-icons'>construction</span></span>", r->uid);
    sb_append(sb, "</td></tr>");
}

static void hud_plist_row_json(PlistRow *r, const cJSON *o) {
    memset(r, 0, sizeof(*r));
    const cJSON *v;
    r->uid = cJSON_IsNumber(v = cJSON_GetObjectItem(o, "uid")) ? v->valueint : 0;
    snprintf(r->name, sizeof(r->name), "%s", cJSON_IsString(v = cJSON_GetObjectItem(o, "name")) ? v->valuestring : "");
    snprintf(r->clan, sizeof(r->clan), "%s", cJSON_IsString(v = cJSON_GetObjectItem(o, "clan")) ? v->valuestring : "");
    r->ping = cJSON_IsNumber(v = cJSON_GetObjectItem(o, "ping")) ? v->valueint : 0;
    r->account_id = cJSON_IsNumber(v = cJSON_GetObjectItem(o, "account")) ? v->valueint : 0;
    r->level = cJSON_IsNumber(v = cJSON_GetObjectItem(o, "level")) ? v->valueint : r->account_id > 0;
    r->hacker = cJSON_IsTrue(cJSON_GetObjectItem(o, "hacker"));
    r->host = cJSON_IsTrue(cJSON_GetObjectItem(o, "host"));
    r->speaking = cJSON_IsTrue(cJSON_GetObjectItem(o, "speaking"));
    r->chat_muted = cJSON_IsTrue(cJSON_GetObjectItem(o, "chatMuted"));
    r->voice_muted = cJSON_IsTrue(cJSON_GetObjectItem(o, "voiceMuted"));
}

static void hud_plist_row_of(const Client *client, const Player *p, PlistRow *row) {
    const NetPlayerInfoPacket *info = player_accounts_get(p->uid);
    memset(row, 0, sizeof(*row));
    row->uid = p->uid;
    snprintf(row->name, sizeof(row->name), "%s", hud_player_name(client, p));
    if (info && info->account_id > 0) snprintf(row->clan, sizeof(row->clan), "%s", info->clan);
    row->ping = p->ping;
    row->account_id = info ? info->account_id : 0;
    row->level = info ? info->level : 0;
    row->hacker = info && (info->flags & NET_PLAYER_INFO_HACKER);
    row->host = info && (info->flags & NET_PLAYER_INFO_HOST);
    row->speaking = voice_is_speaking(p->name);
}

static char *hud_plist_rows(void) {
    const Client *client = menu_ui()->client;
    const size_t count = client && client->game.players ? client->game.player_count : 0;
    StrBuf sb = {0};
    sb_append(&sb, "<table class='pListTable' style='margin-top: 8px;overflow-y: scroll;height: calc(100% - 75px);'><tbody>");
    // isLobbyHost: this client runs the game (offline / hosted), like menu_ui.c's idle kick
    const int remote = client && client->net_connected && !client->offline && !client->game.is_local;
    const char *host = client ? client->net_address : "";
    const int prod = remote && host[0] && strcmp(host, "127.0.0.1") && strcmp(host, "localhost") && strncmp(host, "192.168.", 8);
    const Player *me = client ? client->me : NULL;
    const cJSON *scenario = hud_votekick_override(); // --render-scenario state.playerList
    int dev = 0, is_host = 0;
    hud_plist_staff(client, &dev, &is_host);
    PlistRow row;
    // the K.P.D officer's suspect first (kpdData.sus; a scenario row with "sus": true)
    int sus = hud_votekick_kpd_sus(client);
    if (scenario) {
        const cJSON *o;
        sus = -1;
        cJSON_ArrayForEach(o, scenario) {
            if (!cJSON_IsTrue(cJSON_GetObjectItem(o, "sus"))) continue;
            hud_plist_row_json(&row, o);
            sus = row.uid;
            hud_plist_suspect_row(&sb, client, &row);
            break;
        }
    } else if (sus >= 0) {
        const Player *p = NULL;
        for (size_t i = 0; i < count && !p; i++) {
            if (client->game.players[i] && client->game.players[i]->uid == sus) p = client->game.players[i];
        }
        if (p) {
            hud_plist_row_of(client, p, &row);
            hud_plist_suspect_row(&sb, client, &row);
        } else sus = -1;
    }
    if (me || scenario) {
        sb_append(&sb, "<tr><td class='pListName'>");
        if (is_host) sb_append(&sb, "<i class='material-icons' style='font-size:25px'>stars</i>"); // isHost
        const NetPlayerInfoPacket *info = me ? player_accounts_get(me->uid) : NULL;
        const AccountInfo *acc = account_get();
        // the local row is the account's alias when signed in (v10 Game.players alias of the account)
        sb_append_esc(&sb, acc ? acc->name : me ? hud_player_name(client, me) : client ? client->menu_name : "");
        hud_plist_clan(&sb, info && info->account_id > 0 ? info->clan : acc ? acc->clan : NULL);
        sb_append(&sb, "</td><td class='pListActions'>");
        if (spectate_active()) sb_append(&sb, "<span onmouseenter='playTick()' title='Clear Focus' class='punishButton kick' onclick='clearFocus()'>Clear Focus</span>");
        if (dev && me) sb_appendf(&sb, "<span onmouseenter='playTick()' class='punishButton kill' onclick='punishPlayer(\"%d\", 3)'>Kill</span>", me->uid);
        if (dev) sb_append(&sb, "<span onmouseenter='playTick()' class='punishButton tpall' onclick='punishPlayer(null, 9)'>TPAll</span>");
        sb_append(&sb, "</td></tr>");
    }
    if (scenario) {
        const cJSON *o;
        cJSON_ArrayForEach(o, scenario) {
            hud_plist_row_json(&row, o);
            if (row.uid != sus && hud_plist_matches(row.name)) hud_plist_row(&sb, client, &row, 1);
        }
    }
    const int bots_as_ai = client && client->game.mode && client->game.mode->config.bots_as_ai;
    for (size_t i = 0; !scenario && i < count; i++) {
        const Player *p = client->game.players[i];
        if (!p || (prod && p == me)) continue; // Game.players.list.filter(ho => !config.isProd || !ho.isYou)
        if (p->uid == sus || (p->is_bot && bots_as_ai)) continue; // the suspect's row is above; botsAsAI: bots are AIs
        hud_plist_row_of(client, p, &row);
        if (!hud_plist_matches(row.name)) continue;
        hud_plist_row(&sb, client, &row, 0);
    }
    sb_append(&sb, "</tbody></table>");
    return sb_finish(&sb);
}

// The alias of player `uid` (Game.players.findById(id).alias), NULL when not in the game
static const char *hud_plist_alias(const int uid) {
    const Client *client = menu_ui()->client;
    for (size_t i = 0; client && client->game.players && i < client->game.player_count; i++) {
        const Player *p = client->game.players[i];
        if (p && p->uid == uid) return hud_player_name(client, p);
    }
    return NULL;
}

char *hud_ui_player_list_html(void) {
    const Client *client = menu_ui()->client;
    const size_t count = client && client->game.players ? client->game.player_count : 0;
    size_t listed = 0;
    for (size_t i = 0; i < count; i++) listed += client->game.players[i] != NULL;
    // --render-scenario state.playerList stands in for the lobby's players: v10's list.length counts them
    const cJSON *scenario = hud_votekick_override();
    if (scenario) listed = (size_t) cJSON_GetArraySize(scenario) + 1; // + the local player's row
    StrBuf sb = {0};
    if (listed) {
        sb_appendf(&sb, "<div id='playerSearchH' class='setHed'>%s [%zu]<div id='plySearchH'><input id='plySearch' type='text' ",
                   I18N("windows.players.header", "Player List"), listed);
        if (g_plist_search[0]) {
            sb_append(&sb, "value='");
            sb_append_esc(&sb, g_plist_search);
            sb_append(&sb, "' ");
        }
        sb_append(&sb, "placeholder='Search Player' oninput='windows[22].searchPlayers(this)'></div></div>");
    } else {
        sb_appendf(&sb, "<div style='margin-top:0px;' class='setHed'><center>%s<center></div>", I18N("windows.players.none", "No players currently..."));
    }
    char *rows = hud_plist_rows();
    sb_append(&sb, "<div id='playerListH'>");
    sb_append(&sb, rows);
    sb_append(&sb, "</div>");
    free(rows);
    return sb_finish(&sb);
}

// FNV-1a over the board markup: rewrite #leaderContainerD only when it changed.
static unsigned hud_hash(const char *text) {
    unsigned h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *) text; *p; p++) h = (h ^ *p) * 16777619u;
    return h;
}

static void hud_feed_name_span(char *out, const size_t size, size_t *len, const char *name, const int is_you, const int is_ally) {
    if (is_you) {
        hb_append(out, size, len, "<span style='color:" FEED_COLOR_YOU "'>You</span>");
        return;
    }
    hb_append(out, size, len, "<span style='color:%s'>", is_ally ? FEED_COLOR_ALLY : FEED_COLOR_ENEMY);
    char shown[PLAYER_NAME_SIZE];
    hb_append_fixed(out, size, len, streamer_name(name, shown, sizeof(shown))); // fixStr(scrambleS(alias)) in streamer mode
    hb_append(out, size, len, "</span>");
}

// v7 weaponConfig icnPad by icon (src/data/weapons.js; margin-right of the .weaponChatIcon);
// icon_0 is the Combat Knife, a melee weapon: f2024 shows the killer's melee skin icon
// (default knife = melee icon_0, vA653[0] = 5 px) as .meleeChatIcon instead.
static const struct {
    const char *icon;
    int pad;
} FEED_ICON_PAD[] = {
    {"icon_1", 9}, {"icon_2", 5}, {"icon_3", -15}, {"icon_4", 0}, {"icon_5", -10}, {"icon_6", 10}, {"icon_7", 10},
    {"icon_8", 10}, {"icon_9", 10}, {"icon_10", -4}, {"icon_11", -10}, {"icon_12", -8}, {"icon_14", 9},
    {"icon_15", 9}, {"icon_16", 10}, {"icon_17", -10}, {"icon_18", 9}, {"icon_19", 5}, {"icon_20", 9},
    {"icon_21", -10}, {"icon_22", -10}, {"icon_23", 10}, {"icon_24", -15}, {"icon_25", -15}, {"icon_26", 10},
    {"icon_27", 10}, {"icon_28", -4}, {"icon_29", 9},
    {"icon_30", 9}, {"icon_31", 10}, // v10.0.2 Boulder / Compressor icnPad (v10:101851, 101940)
};
#define FEED_MELEE_ICON "icon_0"
#define FEED_MELEE_PAD 5           // vA653[0]: default knife skin

static int hud_feed_icon_pad(const char *icon) {
    for (size_t i = 0; i < sizeof(FEED_ICON_PAD) / sizeof(FEED_ICON_PAD[0]); i++) {
        if (!strcmp(FEED_ICON_PAD[i].icon, icon)) return FEED_ICON_PAD[i].pad;
    }
    return 0; // gC.icnPad || 0
}

// f2024: "<div class='killfeedItem'><span class='killfeedMsg'>" in #killFeed; with Old
// Scoreboard (`chat`) "<div class='chatItem[ chatTextOutline]'[ style='background-color:
// rgba(0, 0, 0, op)']><span class='chatMsg'>" (chatTextOutline / chatBGOp).
static void hud_feed_open(char *out, const size_t size, size_t *len, const int chat) {
    const ClientRenderSettings *rs = client_render_settings();
    if (!chat) {
        hb_append(out, size, len, "<div class='killfeedItem'><span class='killfeedMsg'>");
        return;
    }
    hb_append(out, size, len, "<div class='chatItem%s'", rs->chat_text_outline ? " chatTextOutline" : "");
    if (fabsf(rs->chat_bg_opacity - CHAT_BG_OPACITY_DEFAULT) > 1e-4f)
        hb_append(out, size, len, " style='background-color: rgba(0, 0, 0, %g)'", (double) rs->chat_bg_opacity);
    hb_append(out, size, len, "><span class='chatMsg'>");
}

// f2024(killer, info, killerHtml, victimHtml): inner markup of a kill row. `killer` NULL = the victim
// killed itself (hC = ""): only the weapon and the victim.
// `streak` >= 0: info.stk (a Nuke kill): the streak's icon instead of the weapon's.
static void hud_feed_row_html(const int chat, const char *killer, const int killer_is_you, const char *victim,
                              const int victim_is_you, const char *weapon_icon, const int headshot, const int streak,
                              char *out, const size_t size) {
    size_t len = 0;
    out[0] = 0;
    hud_feed_open(out, size, &len, chat);
    if (killer || killer_is_you) hud_feed_name_span(out, size, &len, killer ? killer : "", killer_is_you, 0);
    if (streak >= 0) { // info.stk != null: <img class='streakChatIcon' src='/img/streaks/<stk>.png' />
        hb_append(out, size, &len, "<img class='streakChatIcon' src='img/streaks/%d.png' />", streak);
        if (headshot) hb_append(out, size, &len, "<img class='headShotChatIcon' src='img/headshot_0.png'>");
    } else if (weapon_icon && !strcmp(weapon_icon, FEED_MELEE_ICON)) {
        hb_append(out, size, &len, "<img style='opacity:0.7;margin-right:%dpx;' class='meleeChatIcon' src='textures/melee/icon_0.png' />",
                  FEED_MELEE_PAD);
    } else if (weapon_icon && weapon_icon[0]) {
        char kn_src[96]; // KN-only art (nettspend.h): ui/img/weapons/<stem>.png instead of textures/weapons/
        hb_append(out, size, &len, "<img style='opacity:0.7;margin-right:%dpx;' class='weaponChatIcon' src='", hud_feed_icon_pad(weapon_icon));
        if (nettspend_weapon_icon_src(weapon_icon, kn_src, sizeof(kn_src))) hb_append(out, size, &len, "%s", kn_src);
        else {
            hb_append(out, size, &len, "textures/weapons/");
            hb_append_escaped(out, size, &len, weapon_icon);
            hb_append(out, size, &len, ".png");
        }
        hb_append(out, size, &len, "' />");
        if (headshot) hb_append(out, size, &len, "<img class='headShotChatIcon' src='img/headshot_0.png'>");
    } else {
        hb_append(out, size, &len, " "); // unknown weapon (no weaponConfig icon): keep the names apart
    }
    hud_feed_name_span(out, size, &len, victim, victim_is_you, 0);
    hb_append(out, size, &len, "</span></div><br>");
}

// The suicide row (no killer): gE(null, .., victim + " committed " + fixStr(suicide)).
static void hud_feed_suicide_html(const int chat, const char *victim, const int victim_is_you, const char *what,
                                  char *out, const size_t size) {
    size_t len = 0;
    out[0] = 0;
    hud_feed_open(out, size, &len, chat);
    hud_feed_name_span(out, size, &len, victim, victim_is_you, 0);
    hb_append(out, size, &len, " committed ");
    hb_append_fixed(out, size, &len, what);
    hb_append(out, size, &len, "</span></div><br>");
}

// UI.updateCrosshair size: max(58, spread * 1.1 * crosshairScale).
static double hud_cross_size(const int spread, const double ui_height) {
    const double size = (double) spread * CROSS_MLT * (ui_height > 0.0 ? ui_height : UI_DEFAULT_HEIGHT) / UI_DESIGN_HEIGHT;
    return size > CROSS_MIN ? size : CROSS_MIN;
}

/* ------------------------------------------------------------ dom helpers */

static void dom_display(const char *id, const char *value) {
    const HtmlElem el = html_dom_by_id(id);
    if (el) html_dom_set_display(el, value);
}

static void dom_text(const char *id, const char *value) {
    const HtmlElem el = html_dom_by_id(id);
    if (el) html_dom_set_text(el, value);
}

static void dom_text_int(const char *id, const int value) {
    char text[16];
    snprintf(text, sizeof(text), "%d", value);
    dom_text(id, text);
}

static void dom_style(const char *id, const char *prop, const char *value) {
    const HtmlElem el = html_dom_by_id(id);
    if (el) html_dom_set_style(el, prop, value);
}

/* ------------------------------------------------------------ value sources */

static int hud_in_game(const Client *client) {
    return client->in_game && client->me && client->me->active;
}

static int hud_connected(const Client *client) {
    return client->game.is_local || client->offline || client->net_connected;
}

static const char *hud_map_name(const Client *client) {
    if (!client->game.map || !client->game.map->raw_data) return "";
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(client->game.map->raw_data, "name");
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

// Only FFA is wired up as a mode so far.
// The running mode's display name (GameMode config, same source as the menu's top bar).
static const char *hud_mode_name(const Client *client) {
    if (!client->game.mode) return "";
    return client->game.mode->config.name ? client->game.mode->config.name : "";
}

// Map "welMsg" (Krunker shows it in chat on join; only some maps define it).
static const char *hud_welcome_msg(const Client *client) {
    if (!client->game.map || !client->game.map->raw_data) return "";
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(client->game.map->raw_data, "welMsg");
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

// Krunker shows the server's match clock as "MM:SS" (utils.getTime): the round clock
// from the server (TIMER) once one arrived, else the configured match length.
static void hud_timer(const Client *client, char *out, const size_t size) {
    if (client->net_round_seconds >= 0) {
        hud_timer_text(client->net_round_seconds, out, size);
        return;
    }
    hud_timer_text(client->game.config.game_time > 0 ? client->game.config.game_time * 60 : 0, out, size);
}

// Krunker reloadUIAnim: ammoDisplay background-size = reloadTimer / (reload * reSpd) * 100, rounded.
static int hud_reload_percent(const Player *me, const Game *game) {
    if (!me->weapon || me->reload_timer <= 0.0f || me->weapon->reload_time <= 0.0f) return 0;

    const float reload_speed = game->config.reload_speed > 0.0f ? game->config.reload_speed : 1.0f;
    const float progress = me->reload_timer / (me->weapon->reload_time * reload_speed);

    return (int) lroundf(CLAMP(progress, 0.0f, 1.0f) * 100.0f);
}

// Krunker player.spread: max(minSpread, (weapon.spread - crouch term + recoil term +
// spreadPlus) * aimVal) with aimVal 1 = hip, 0 = aimed. shared/src/player.c does
// not update Player.spread yet (TODO there), so the resting part of the formula is
// evaluated here; the movement/recoil terms follow once player.c tracks them.
static float hud_spread(const Player *me) {
    const Weapon *weapon = me->weapon;
    if (!weapon || weapon->no_spread) return 0.0f;
    if (me->spread > 0.0f) return me->spread;

    const float hip = 1.0f - CLAMP(me->aim_val, 0.0f, 1.0f);
    return fmaxf(weapon->min_spread, weapon->spread * hip);
}

// The skin a loadout slot wears (players.js getSkinForLoadout(slot), the melee skin on the melee
// weapon): player.skins matched to the slot's weapon, the rule player_mesh.c player_slot_skin uses
// for the model. NULL when none.
static const ViewerSkin *hud_slot_skin(const Player *me, const int slot, const Weapon *weapon) {
    if (!weapon) return NULL;
    if (weapon->melee) {
        const ViewerSkin *skin = viewer_skin(me->skins.melee);
        return skin && skin->type == 3 ? skin : NULL;
    }
    const ViewerSkin *skin = viewer_skin(slot == 0 ? me->skins.primary : slot == 1 ? me->skins.secondary : -1);
    return skin && skin->type == 0 && me->loadout && skin->weapon - 1 == me->loadout[slot] ? skin : NULL;
}

static void hud_loadout(const Player *me, HudUiExtra *extra) {
    const Game *game = me->game;
    const size_t weapon_count = game->weapon_count ? game->weapon_count : rules_weapon_count();
    const Weapon **weapons = game->weapons ? (const Weapon**) game->weapons : (const Weapon**) rules_weapons();

    extra->loadout_size = 0;
    extra->loadout_index = me->loadout_index;
    extra->can_throw = me->can_throw;
    const ViewerSkin *melee = viewer_skin(me->skins.melee);
    extra->melee_icon = melee && melee->type == 3 && melee->id > 0 ? melee->id : 0;
    extra->loadout_base = player_loadout_base(me);
    const int shown = extra->loadout_base < me->loadout_size ? HUD_UI_LOADOUT_ALL : HUD_UI_LOADOUT_MAX; // every slot with extras
    for (int i = 0; i < me->loadout_size && i < shown; i++) {
        const int id = me->loadout ? me->loadout[i] : -1;
        const Weapon *weapon = id >= 0 && (size_t) id < weapon_count ? weapons[id] : NULL;
        extra->skin[extra->loadout_size] = hud_slot_skin(me, i, weapon);
        extra->loadout[extra->loadout_size++] = weapon;
    }
}

static int hud_scoreboard_key(const Client *client) {
    // Krunker shows #newLeaderDisplay while sBoardKey (Tab by default) is held; the
    // keybinds (ui_controls.c) track the rebindable key and ignore it while typing in chat.
    return client->window && !html_ui_is_menu_open() && ui_controls_scoreboard_held();
}

static int hud_ammo(const Player *me) {
    if (!me->ammo || me->loadout_index < 0 || me->loadout_index >= me->loadout_size) return 0;
    return (int) me->ammo[me->loadout_index];
}

static int hud_health_segments(const Client *client, const Player *me) {
    const Game *game = &client->game;
    const ClassConfig *class = game->classes && me->class_index >= 0 && (size_t) me->class_index < game->class_count ? &game->classes[me->class_index] : NULL;
    return class && class->health_segments > 0 ? class->health_segments : 1;
}

// index.js gameLoop: currentFps = frames in the last 1000 ms (sliding window, counted by
// client_render_fps), and ingameFPS is rewritten at most every 100 ms.
static void hud_sample_fps(HtmlHudState *hud, const double now, int *changed) {
    if (hud->fps_window_start > 0.0 && now - hud->fps_window_start < HUD_UI_FPS_INTERVAL) return;
    const int fps = client_render_fps();
    if (!fps) return; // `currentFps &&`
    hud->fps_window_start = now;
    if (fps != hud->fps) *changed = 1;
    hud->fps = fps;
}

/* ------------------------------------------------------------ crosshair / hit marker */

static void on_ui_base_resize(const HtmlElem base, void *user) {
    (void) user;
    g_hud_ui.ui_height = html_dom_offset_height(base);
    g_hud_ui.cross_dirty = 1;
}

// #uiBase is scaled to fit the viewport (menu scaleBase); its layout height is what
// Krunker's overlay canvas math works in.
static float hud_ui_height(void) {
    const HtmlElem base = html_dom_by_id("uiBase");
    char text[32];
    float height = 0.0f;

    if (base && html_dom_get_style(base, "height", text, sizeof(text)) > 0) height = strtof(text, NULL);
    if (base && height <= 0.0f) height = html_dom_offset_height(base);
    return height > 0.0f ? height : (float) UI_DEFAULT_HEIGHT;
}

static HtmlElem hud_ensure_part(HtmlElem *cache, const char *id, const char *arm_class, const char *const sides[4]) {
    if (*cache) return *cache;

    HtmlElem el = html_dom_by_id(id);
    if (!el) {
        const HtmlElem host = html_dom_by_id("inGameUI");
        if (!host) return 0;

        el = html_dom_create("div", id);
        for (int i = 0; sides && i < 4; i++) {
            char cls[64];
            snprintf(cls, sizeof(cls), "%s %s-%s", arm_class, arm_class, sides[i]);
            const HtmlElem arm = html_dom_create("div", NULL);
            html_dom_set_attr(arm, "class", cls);
            html_dom_append(el, arm);
        }
        html_dom_append(host, el);
    }
    *cache = el;
    return el;
}

// #knCrosshair; its layers are (re)built by hud_crosshair_build() from the settings.
static HtmlElem hud_ensure_crosshair(void) {
    return hud_ensure_part(&g_hud_ui.crosshair, "knCrosshair", NULL, NULL);
}

static HtmlElem hud_ensure_hitmarker(void) {
    static const char *const sides[4] = {"t", "b", "l", "r"};
    return hud_ensure_part(&g_hud_ui.hitmarker, "knHitmarker", "knHitBar", sides);
}

// "#rgb" / "#rrggbb" -> r, g, b (aU.hexToRGB); 0 when `hex` is something else.
static int hud_hex_rgb(const char *hex, int rgb[3]) {
    if (!hex || hex[0] != '#') return 0;
    const size_t n = strlen(hex + 1);
    unsigned v[6];
    for (size_t i = 0; i < n && i < 6; i++) {
        const char c = hex[1 + i];
        if (c >= '0' && c <= '9') v[i] = (unsigned) (c - '0');
        else if (c >= 'a' && c <= 'f') v[i] = (unsigned) (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v[i] = (unsigned) (c - 'A' + 10);
        else return 0;
    }
    for (int i = 0; i < 3; i++) {
        if (n == 3) rgb[i] = (int) (v[i] * 17);
        else if (n == 6) rgb[i] = (int) (v[2 * i] * 16 + v[2 * i + 1]);
        else return 0;
    }
    return 1;
}

// A colour setting as a CSS colour with `alpha` (the overlay's globalAlpha *= 0.5 shadow).
static void hud_css_color(const char *hex, const double alpha, char *out, const size_t size) {
    int rgb[3];
    if (hud_hex_rgb(hex, rgb)) {
        if (alpha >= 1.0) snprintf(out, size, "rgb(%d,%d,%d)", rgb[0], rgb[1], rgb[2]);
        else snprintf(out, size, "rgba(%d,%d,%d,%g)", rgb[0], rgb[1], rgb[2], alpha);
        return;
    }
    size_t len = 0;
    out[0] = 0;
    hb_append_escaped(out, size, &len, hex && hex[0] ? hex : "#fff");
}

// One filled rect (overlay px, relative to the screen centre).
static void hud_cross_rect(char *out, const size_t size, size_t *len, const double x, const double y, const double w, const double h, const char *color) {
    hb_append(out, size, len, "<div style='position:absolute;left:%gpx;top:%gpx;width:%gpx;height:%gpx;background-color:%s'></div>", x, y, w, h, color);
}

// One stroked or filled square/ellipse of half-size `r` (canvas stroke centred on the path).
static void hud_cross_shape(char *out, const size_t size, size_t *len, const double r, const double line, const int ellipse, const int fill, const char *color) {
    const char *radius = ellipse ? "border-radius:50%;" : "";
    if (fill) {
        hb_append(out, size, len, "<div style='position:absolute;left:%gpx;top:%gpx;width:%gpx;height:%gpx;%sbackground-color:%s'></div>",
            -r, -r, 2.0 * r, 2.0 * r, radius, color);
        return;
    }
    const double outer = r + line / 2.0;
    hb_append(out, size, len, "<div style='position:absolute;box-sizing:border-box;left:%gpx;top:%gpx;width:%gpx;height:%gpx;%sborder:%gpx solid %s'></div>",
        -outer, -outer, 2.0 * outer, 2.0 * outer, radius, line, color);
}

// game-overlay ao(): one precision-crosshair bar (or the dot) with the optional 1px
// shadow stroke (canvas lineWidth 1 centred on the edge -> a 1px border on a 1px larger box).
static void hud_cross_bar(char *out, const size_t size, size_t *len, const double x, const double y, const double w, const double h,
                          const char *color, const char *stroke) {
    if (!stroke) {
        hud_cross_rect(out, size, len, x, y, w, h, color);
        return;
    }
    hb_append(out, size, len, "<div style='position:absolute;box-sizing:border-box;left:%gpx;top:%gpx;width:%gpx;height:%gpx;background-color:%s;border:1px solid %s'></div>",
        x - 0.5, y - 0.5, w + 1.0, h + 1.0, color, stroke);
}

// Settings the crosshair markup depends on (rebuild when any changes).
static void hud_cross_sig(const ClientRenderSettings *rs, char *out, const size_t size) {
    snprintf(out, size, "%d|%d|%s|%s|%s|%g|%g|%g|%g|%d", rs->crosshair_type, rs->crosshair_style, rs->crosshair_color,
        rs->crosshair_shadow, rs->crosshair_image, rs->crosshair_shadow_thickness, rs->crosshair_thick,
        rs->crosshair_len, rs->crosshair_gap, rs->crosshair_dot);
}

// The overlay's crosshair (91.js, `crosshairType > 0` branch) as DOM layers in #uiBase
// px, centred on #knCrosshair:
//   #knCrossDef  type 1/3: the default four 4x21 bars on the edges of a square of
//                crosshairScale (--kn-cross-size, ui/css/hud.css) with an() shadows
//   #knCrossCus  type 2/3: style 0 cross (bm x 2bn bars), 1 circle, 2 filled circle,
//                3 square, 4 filled square (an(), lineWidth bm / 2); type 4: the image
//                at its natural size; type 5: precision ao() bars (+ dot)
// Colours: crosshairColor; shadows crosshairShadow at 0.5 alpha (an()) or full alpha (ao()),
// drawn only when crosshairShadowThickess is non-zero (the overlay only tests it).
static void hud_crosshair_build(const ClientRenderSettings *rs) {
    HudUi *ui = &g_hud_ui;
    const HtmlElem ch = ui->crosshair;
    if (!ch) return;

    const int type = rs->crosshair_type;
    const int shadow_on = rs->crosshair_shadow_thickness != 0.0f;
    const double bm = rs->crosshair_thick, bn = rs->crosshair_len;
    char color[64], shadow[64], stroke[64], html[4096];
    size_t len = 0;
    html[0] = 0;
    hud_css_color(rs->crosshair_color, 1.0, color, sizeof(color));
    hud_css_color(rs->crosshair_shadow, 0.5, shadow, sizeof(shadow));
    hud_css_color(rs->crosshair_shadow, 1.0, stroke, sizeof(stroke));

    // default bars (type 1, and under the custom layer for type 3)
    hb_append(html, sizeof(html), &len, "<div id='knCrossDef' style='position:absolute;left:0;top:0'>");
    if (type == 1 || type == 3) {
        // crosshairDot (default on): the 6x6 centre square, no shadow stroke (render.js type 1 / 3)
        if (rs->crosshair_dot) hb_append(html, sizeof(html), &len, "<div class='knCrossDot' style='background-color:%s'></div>", color);
        static const char *const sides[4] = {"t", "r", "b", "l"};
        for (int i = 0; i < 4; i++) {
            hb_append(html, sizeof(html), &len, "<div class='knCrossArm knCrossArm-%s' style='background-color:%s;box-shadow:", sides[i], color);
            // v10 strokes its 6x23 bars 1px (ui/css/hud.css)
            if (shadow_on) hb_append(html, sizeof(html), &len, "0 0 0 1px %s'></div>", shadow);
            else hb_append(html, sizeof(html), &len, "none'></div>");
        }
    }
    hb_append(html, sizeof(html), &len, "</div><div id='knCrossCus' style='position:absolute;left:0;top:0'>");
    if (type == 2 || type == 3) {
        const int style = rs->crosshair_style;
        if (style == 0) {
            // an(V, shadow), an(H, shadow), an(V, no shadow)
            if (shadow_on) hud_cross_rect(html, sizeof(html), &len, -bm * 0.5 - 2.0, -bn - 2.0, bm + 4.0, bn * 2.0 + 4.0, shadow);
            hud_cross_rect(html, sizeof(html), &len, -bm * 0.5, -bn, bm, bn * 2.0, color);
            if (shadow_on) hud_cross_rect(html, sizeof(html), &len, -bn - 2.0, -bm / 2.0 - 2.0, bn * 2.0 + 4.0, bm + 4.0, shadow);
            hud_cross_rect(html, sizeof(html), &len, -bn, -bm / 2.0, bn * 2.0, bm, color);
            hud_cross_rect(html, sizeof(html), &len, -bm * 0.5, -bn, bm, bn * 2.0, color);
        } else {
            const int ellipse = style == 1 || style == 2;
            const int fill = style == 2 || style == 4;
            if (shadow_on) hud_cross_shape(html, sizeof(html), &len, bn + 2.0, bm / 2.0, ellipse, fill, shadow);
            hud_cross_shape(html, sizeof(html), &len, bn, bm / 2.0, ellipse, fill, color);
        }
    } else if (type == 4 && rs->crosshair_image[0]) {
        hb_append(html, sizeof(html), &len, "<img style='position:absolute;left:0;top:0;transform:translate(-50%%,-50%%)' src='");
        hb_append_escaped(html, sizeof(html), &len, rs->crosshair_image);
        hb_append(html, sizeof(html), &len, "'>");
    } else if (type == 5) {
        const double m = bm / 10.0 * 19.0;              // ao(): aq
        const double l = bn / 10.0 * 19.0;              // |ar|
        const double gap = rs->crosshair_gap * 1.0 + 3.0; // as
        const char *bar_stroke = shadow_on ? stroke : NULL;
        if (rs->crosshair_dot) hud_cross_bar(html, sizeof(html), &len, -m / 2.0, -m / 2.0, m, m, color, NULL);
        hud_cross_bar(html, sizeof(html), &len, m / 2.0 + gap, -m / 2.0, l, m, color, bar_stroke);        // 90
        hud_cross_bar(html, sizeof(html), &len, -(m / 2.0 + gap + l), -m / 2.0, l, m, color, bar_stroke); // -90
        hud_cross_bar(html, sizeof(html), &len, -m / 2.0, -(m / 2.0 + gap + l), m, l, color, bar_stroke); // 0
        hud_cross_bar(html, sizeof(html), &len, -m / 2.0, m / 2.0 + gap, m, l, color, bar_stroke);        // 180
    }
    hb_append(html, sizeof(html), &len, "</div>");

    html_dom_set_html(ch, html);
    html_dom_set_style(ch, "opacity", "1");
    ui->cross_def = html_dom_by_id("knCrossDef");
    ui->cross_cus = html_dom_by_id("knCrossCus");
    ui->cross_opacity[0] = 0;
    ui->cross_def_opacity[0] = 0;
    ui->cross_size[0] = 0;
}

static void hud_cross_set_opacity(const HtmlElem el, const double opacity, char *cache, const size_t size) {
    char op[16];
    fmt_opacity(opacity, op, sizeof(op));
    if (el && strcmp(op, cache) != 0) {
        html_dom_set_style(el, "opacity", op);
        snprintf(cache, size, "%s", op);
    }
}

// UI.updateCrosshair(max(58, spread * 1.1 * crosshairScale), aimVal * (inspecting ? 0 : 1) * ... * (reloading ? 0 : 1))
// (index.js LqKL1 call); `reloading` here is "reloading or inspecting":
// the default bars fade with that opacity; the custom layers use it too unless
// crosshairAlways (1) or crosshairUseOpacity (crosshairOpacity) replace it (type > 1).
// The overlay skips the crosshair while #newLeaderDisplay is inline-block.
static void hud_render_crosshair(const HudUiExtra *extra, const int reloading) {
    HudUi *ui = &g_hud_ui;
    const HtmlElem ch = hud_ensure_crosshair();
    if (!ch) return;

    const ClientRenderSettings *rs = client_render_settings();
    char sig[sizeof(ui->cross_sig)];
    hud_cross_sig(rs, sig, sizeof(sig));
    if (strcmp(sig, ui->cross_sig) != 0 || !ui->cross_cus) {
        hud_crosshair_build(rs);
        snprintf(ui->cross_sig, sizeof(ui->cross_sig), "%s", sig);
    }

    if (ui->cross_dirty || ui->ui_height <= 0.0f) {
        ui->ui_height = hud_ui_height();
        ui->cross_dirty = 0;
    }

    const int type = rs->crosshair_type;
    const double aim = CLAMP(extra->aim_pct, 0, 100) / 100.0;
    const double aim_opacity = (1.0 - aim) * (reloading ? 0.0 : 1.0);
    double opacity = aim_opacity;
    if (type > 1 && rs->crosshair_always) opacity = 1.0;
    if (type > 1 && rs->crosshair_use_opacity) opacity = rs->crosshair_opacity;
    const int shown = type > 0 && !extra->scoreboard && opacity > 0.0 && !ui->dead && !ui->ended; // overlay canvas hidden

    char size[24];
    snprintf(size, sizeof(size), "%.1fpx", hud_cross_size(extra->spread, ui->ui_height));
    if (strcmp(size, ui->cross_size) != 0) {
        html_dom_set_style(ch, "--kn-cross-size", size);
        strcpy(ui->cross_size, size);
    }
    hud_cross_set_opacity(ui->cross_def, aim_opacity, ui->cross_def_opacity, sizeof(ui->cross_def_opacity));
    hud_cross_set_opacity(ui->cross_cus, opacity, ui->cross_opacity, sizeof(ui->cross_opacity));
    if (shown != ui->cross_shown) {
        html_dom_set_display(ch, shown ? "block" : "none");
        ui->cross_shown = shown;
    }
}

// #knCrosshair down (the overlay canvas skips it); hud_render_crosshair shows it again.
static void hud_hide_crosshair(void) {
    if (g_hud_ui.cross_shown == 0) return;
    if (g_hud_ui.crosshair) html_dom_set_display(g_hud_ui.crosshair, "none");
    g_hud_ui.cross_shown = 0;
}

static void hud_hide_hit(void) {
    if (g_hud_ui.hitmarker) html_dom_set_display(g_hud_ui.hitmarker, "none");
    g_hud_ui.hit_active = 0;
}

// game-overlay showHit() per frame: spacing grows hitSet.inc per ms while the opacity
// drops hitSet.fadeOut per ms.
static void hud_hit_step(const double now_ms) {
    HudUi *ui = &g_hud_ui;
    if (!ui->hit_active || !ui->hitmarker) return;

    const double t = now_ms - ui->hit_start_ms;
    const double opacity = ui->hit_opacity - ui->hit_fade * t;
    if (opacity <= 0.0) {
        hud_hide_hit();
        return;
    }

    char text[24];
    snprintf(text, sizeof(text), "%.3f", opacity);
    html_dom_set_style(ui->hitmarker, "opacity", text);
    snprintf(text, sizeof(text), "%.1fpx", ui->hit_spacing + ui->hit_inc * t);
    html_dom_set_style(ui->hitmarker, "--kn-hit-spacing", text);
}

void hud_ui_hit(Client *client, const int kill) {
    HudUi *ui = &g_hud_ui;
    if (!html_ui_available() || ui->visible != 1) return;
    // Dead (a hit credited after the death: a projectile, the kill shot's HIT behind the KILL):
    // v7's game-overlay draws the marker on the overlay canvas, which the death card hides, and
    // its fade runs out before a respawn. #knHitmarker would stay on the death screen instead.
    if (client && !hud_in_game(client)) return;

    // showHit(): hitSet (Display tab hitmarker settings, 134.js hitm*)
    const ClientRenderSettings *rs = client_render_settings();
    if (!rs->hitmarker) return;

    const HtmlElem hm = hud_ensure_hitmarker();
    if (!hm) return;

    char text[64];
    ui->hit_opacity = rs->hitmarker_opacity;
    ui->hit_spacing = rs->hitmarker_spacing;
    ui->hit_inc = rs->hitmarker_anim_size;
    ui->hit_fade = rs->hitmarker_fade;
    snprintf(text, sizeof(text), "%gpx", rs->hitmarker_length * (kill ? HIT_KILL_MLT : 1.0));
    html_dom_set_style(hm, "--kn-hit-len", text);
    snprintf(text, sizeof(text), "%gpx", rs->hitmarker_thickness);
    html_dom_set_style(hm, "--kn-hit-thick", text);
    hud_css_color(kill ? rs->hitmarker_kill_color : rs->hitmarker_color, 1.0, text, sizeof(text));
    html_dom_set_style(hm, "--kn-hit-color", text);
    snprintf(text, sizeof(text), "%gpx", rs->hitmarker_spacing);
    html_dom_set_style(hm, "--kn-hit-spacing", text);
    fmt_opacity(ui->hit_opacity, text, sizeof(text));
    html_dom_set_style(hm, "opacity", text);
    html_dom_set_display(hm, "block");

    ui->hit_active = 1;
    ui->hit_start_ms = (g_hud_clock_s >= 0.0 ? g_hud_clock_s : client && client->window ? glfwGetTime() : 0.0) * 1000.0;
}

/* ------------------------------------------------------------ damage direction (aV.hits) */

// game-overlay am(): a (overlay height / 2) x 83 rect centred on the screen and rotated
// by radToDeg(mfD9(hit.x, hit.z, me.x, me.z) + xDr); its horizontal gradient is clear up
// to 85 % and fades rgba(255,63,63,.8) -> 0 over the outer 15 %, drawn at alpha
// life / hitLife. Krunker paints it on <canvas id="overlay">; like the crosshair it is a
// div inside #inGameUI here, so overlay px are #uiBase px.
static HtmlElem hud_dmg_element(const int index) {
    DamageHit *hit = &g_hud_ui.dmg[index];
    if (hit->el) return hit->el;

    const HtmlElem host = html_dom_by_id("inGameUI");
    if (!host) return 0;

    char id[32], text[32];
    snprintf(id, sizeof(id), "knDamageHit_%d", index);
    const HtmlElem el = html_dom_create("div", id);
    html_dom_set_style(el, "position", "absolute");
    html_dom_set_style(el, "left", "50%");
    html_dom_set_style(el, "top", "50%");
    snprintf(text, sizeof(text), "%gpx", DMG_HEIGHT);
    html_dom_set_style(el, "height", text);
    snprintf(text, sizeof(text), "%gpx", -DMG_HEIGHT / 2.0);
    html_dom_set_style(el, "margin-top", text);
    html_dom_set_style(el, "pointer-events", "none");
    html_dom_set_style(el, "z-index", "4");
    // background: hud_dmg_set_color() when the hit is shown
    html_dom_set_display(el, "none");
    html_dom_append(host, el);
    hit->el = el;
    hit->shown = 0;
    return el;
}

// hitColor (hexToRGB) into the am() gradient.
static void hud_dmg_set_color(const HtmlElem el) {
    int rgb[3] = {255, 63, 63}; // hitColor default #FF3F3F
    hud_hex_rgb(client_render_settings()->hit_color, rgb);
    char bg[160];
    snprintf(bg, sizeof(bg), "linear-gradient(to right, rgba(0,0,0,0) 85%%, rgba(%d,%d,%d,0.8) 85%%, rgba(%d,%d,%d,0) 100%%)",
        rgb[0], rgb[1], rgb[2], rgb[0], rgb[1], rgb[2]);
    html_dom_set_style(el, "background", bg);
}

static void hud_dmg_hide_all(void) {
    for (int i = 0; i < DMG_MAX; i++) {
        DamageHit *hit = &g_hud_ui.dmg[i];
        if (hit->el && hit->shown) html_dom_set_display(hit->el, "none");
        hit->shown = 0;
    }
    g_hud_ui.dmg_last_ms = -1.0; // hits only age while drawn (Krunker: `at && at.active`)
}

// aV.hits.length = 0 (new round / spawn).
static void hud_dmg_clear(void) {
    hud_dmg_hide_all();
    for (int i = 0; i < DMG_MAX; i++) g_hud_ui.dmg[i].life_ms = 0.0;
}

// Per frame while the HUD is up and the local player alive: age and redraw every hit.
static void hud_dmg_step(const Client *client, const double now_ms) {
    HudUi *ui = &g_hud_ui;
    if (!client_render_settings()->show_hit_ind) { // showHitInd.set(false): hits.length = 0
        if (ui->dmg_last_ms >= 0.0) hud_dmg_clear();
        return;
    }
    double dt = ui->dmg_last_ms >= 0.0 ? now_ms - ui->dmg_last_ms : 0.0;
    if (dt < 0.0) dt = 0.0;
    ui->dmg_last_ms = now_ms;

    const Player *me = client->me;
    const float height = ui->ui_height > 0.0f ? ui->ui_height : hud_ui_height();
    char text[48];

    for (int i = 0; i < DMG_MAX; i++) {
        DamageHit *hit = &ui->dmg[i];
        if (hit->life_ms <= 0.0) continue;

        hit->life_ms -= dt;
        if (hit->life_ms <= 0.0) {
            hit->life_ms = 0.0;
            if (hit->el && hit->shown) html_dom_set_display(hit->el, "none");
            hit->shown = 0;
            continue;
        }

        const HtmlElem el = hud_dmg_element(i);
        if (!el) continue;
        const double deg = (atan2(hit->z - me->position.z, hit->x - me->position.x) + me->direction.y) * 180.0 / M_PI;
        snprintf(text, sizeof(text), "%.2fpx", height / 2.0);
        html_dom_set_style(el, "width", text);
        snprintf(text, sizeof(text), "%.2fpx", -height / 4.0);
        html_dom_set_style(el, "margin-left", text);
        snprintf(text, sizeof(text), "rotate(%.2fdeg)", deg);
        html_dom_set_style(el, "transform", text);
        fmt_opacity(hit->life_ms / HIT_LIFE_MS, text, sizeof(text));
        html_dom_set_style(el, "opacity", text);
        if (!hit->shown) {
            hud_dmg_set_color(el);
            html_dom_set_display(el, "block");
        }
        hit->shown = 1;
    }
}

// The local player took damage from a dealer at x/z (Krunker "h" packet -> aV.hits):
// reuse the first dead entry, else the one closest to expiry.
void hud_ui_damage_from(Client *client, const float x, const float z) {
    (void) client;
    HudUi *ui = &g_hud_ui;
    if (!client_render_settings()->show_hit_ind) return; // showHits off
    int slot = 0;
    for (int i = 0; i < DMG_MAX; i++) {
        if (ui->dmg[i].life_ms <= 0.0) {
            slot = i;
            break;
        }
        if (ui->dmg[i].life_ms < ui->dmg[slot].life_ms) slot = i;
    }
    ui->dmg[slot].life_ms = HIT_LIFE_MS;
    ui->dmg[slot].x = x;
    ui->dmg[slot].z = z;
}

/* ------------------------------------------------------------ damage numbers (dmgNums) */

// World point -> #uiBase fraction (0..1, y down) through the scene camera (row-major
// matrices like vfx.c); 0 when it is outside the view frustum.
static int hud_project(const Client *client, const float x, const float y, const float z, double *sx, double *sy) {
    const Camera *camera = &client->camera;
    const float p[4] = {x, y, z, 1.0f};
    float view[4], clip[4];
    for (int r = 0; r < 4; r++) {
        view[r] = camera->world_inverse_matrix[r * 4] * p[0] + camera->world_inverse_matrix[r * 4 + 1] * p[1] +
                  camera->world_inverse_matrix[r * 4 + 2] * p[2] + camera->world_inverse_matrix[r * 4 + 3] * p[3];
    }
    for (int r = 0; r < 4; r++) {
        clip[r] = camera->projection_matrix[r * 4] * view[0] + camera->projection_matrix[r * 4 + 1] * view[1] +
                  camera->projection_matrix[r * 4 + 2] * view[2] + camera->projection_matrix[r * 4 + 3] * view[3];
    }
    if (clip[3] <= 0.0f) return 0;
    const double nx = clip[0] / clip[3], ny = clip[1] / clip[3], nz = clip[2] / clip[3];
    if (nx < -1.0 || nx > 1.0 || ny < -1.0 || ny > 1.0 || nz < -1.0 || nz > 1.0) return 0; // frustum.containsPoint
    *sx = (nx + 1.0) / 2.0;
    *sy = 1.0 - (ny + 1.0) / 2.0;
    return 1;
}

static void hud_nums_hide_all(void) {
    for (int i = 0; i < DMG_NUM_MAX; i++) {
        DamageNum *num = &g_hud_ui.nums[i];
        if (num->el && num->shown) html_dom_set_display(num->el, "none");
        num->shown = 0;
    }
    g_hud_ui.nums_last_ms = -1.0;
}

// game-overlay NJ2(x, y, z, value, crit): a damage number at the hit point that drifts
// up 0.022 / ms and sideways 0.013 / ms for 1 s, drawn in dmgScale * 30 px GameFont,
// critColor or dmgColor, with a 5px #2e2e2e stroke (text-shadow ring here).
void hud_ui_damage_number(Client *client, const float x, const float y, const float z, const int value, const int crit) {
    (void) client;
    HudUi *ui = &g_hud_ui;
    if (!client_render_settings()->show_dmg) return; // showDMG
    int slot = 0;
    for (int i = 0; i < DMG_NUM_MAX; i++) {
        if (ui->nums[i].life_ms <= 0.0) {
            slot = i;
            break;
        }
        if (ui->nums[i].life_ms < ui->nums[slot].life_ms) slot = i;
    }
    DamageNum *num = &ui->nums[slot];
    num->life_ms = DMG_NUM_LIFE_MS;
    num->x = x;
    num->y = y + 7.0f;
    num->z = z;
    num->dir = (float) (((double) rand() / RAND_MAX) * 2.0 * M_PI - M_PI); // randFloat(-PI, PI)
    num->value = value;
    num->crit = crit;
    if (num->el && num->shown) html_dom_set_display(num->el, "none");
    num->shown = 0;
}

static HtmlElem hud_num_element(const int index) {
    DamageNum *num = &g_hud_ui.nums[index];
    if (num->el) return num->el;
    const HtmlElem host = html_dom_by_id("inGameUI");
    if (!host) return 0;

    char id[32];
    snprintf(id, sizeof(id), "knDmgNum_%d", index);
    const HtmlElem el = html_dom_create("div", id);
    html_dom_set_style(el, "position", "absolute");
    html_dom_set_style(el, "pointer-events", "none");
    html_dom_set_style(el, "z-index", "4");
    html_dom_set_style(el, "white-space", "nowrap");
    html_dom_set_style(el, "font-family", "gamefont");
    html_dom_set_style(el, "transform", "translate(-50%, -75%)"); // centred on the point, alphabetic baseline
    html_dom_set_style(el, "text-shadow", "2px 0 #2e2e2e, -2px 0 #2e2e2e, 0 2px #2e2e2e, 0 -2px #2e2e2e, "
        "2px 2px #2e2e2e, -2px -2px #2e2e2e, 2px -2px #2e2e2e, -2px 2px #2e2e2e");
    html_dom_set_display(el, "none");
    html_dom_append(host, el);
    num->el = el;
    num->shown = 0;
    return el;
}

static void hud_nums_step(const Client *client, const double now_ms) {
    HudUi *ui = &g_hud_ui;
    const ClientRenderSettings *rs = client_render_settings();
    double dt = ui->nums_last_ms >= 0.0 ? now_ms - ui->nums_last_ms : 0.0;
    if (dt < 0.0) dt = 0.0;
    ui->nums_last_ms = now_ms;

    for (int i = 0; i < DMG_NUM_MAX; i++) {
        DamageNum *num = &ui->nums[i];
        if (num->life_ms <= 0.0) continue;
        num->life_ms -= dt;
        double sx = 0.0, sy = 0.0;
        int visible = 0;
        if (num->life_ms <= 0.0) {
            num->life_ms = 0.0;
        } else {
            num->y += (float) (dt * 0.022);
            num->x += (float) (dt * 0.013 * cos(num->dir));
            num->z += (float) (dt * 0.013 * sin(num->dir));
            visible = rs->show_dmg && hud_project(client, num->x, num->y, num->z, &sx, &sy);
        }
        if (!visible) {
            if (num->el && num->shown) html_dom_set_display(num->el, "none");
            num->shown = 0;
            continue;
        }
        const HtmlElem el = hud_num_element(i);
        if (!el) continue;
        char text[64];
        snprintf(text, sizeof(text), "%.3f%%", sx * 100.0);
        html_dom_set_style(el, "left", text);
        snprintf(text, sizeof(text), "%.3f%%", sy * 100.0);
        html_dom_set_style(el, "top", text);
        if (!num->shown) {
            snprintf(text, sizeof(text), "%gpx", rs->dmg_scale * 30.0);
            html_dom_set_style(el, "font-size", text);
            // dmgColor is a switch+colour row (val COLORS.damage #DE292F); ClientRenderSettings
            // keeps only the switch, so on = #DE292F, off = white. Crits use critColor.
            html_dom_set_style(el, "color", num->crit ? rs->crit_color : rs->dmg_color ? "#DE292F" : "#ffffff");
            snprintf(text, sizeof(text), "%d", num->value);
            html_dom_set_text(el, text);
            html_dom_set_display(el, "block");
            num->shown = 1;
        }
    }
}

/* ------------------------------------------------------------ speedometer (Qp9) */

// game-overlay Qp9 (showSpeed): "<speed> <max>" centred at speedOffX * 10 % / speedOffY *
// 10 % of the overlay, speedScale * 10 px GameFont at speedOpacity, speedColor / speedColorMax.
// The speed is the 3D velocity length * 1000 (units / s), refreshed every 100 ms; 0 while
// dead or move-locked. Max resets each spawn (aV.Qp9.max = 0).
static void hud_speed_step(const Client *client, const double now_ms) {
    HudUi *ui = &g_hud_ui;
    const ClientRenderSettings *rs = client_render_settings();
    const int on = rs->show_speed && ui->visible == 1 && !ui->dead && !ui->ended;
    if (!on) {
        if (ui->speed && ui->speed_shown) html_dom_set_display(ui->speed, "none");
        ui->speed_shown = 0;
        return;
    }
    if (!ui->speed) {
        const HtmlElem host = html_dom_by_id("inGameUI");
        if (!host) return;
        ui->speed = html_dom_create("div", "knSpeed");
        html_dom_set_html(ui->speed, "<span id='knSpeedCur'>0</span><span id='knSpeedMax' style='margin-left:5px'>0</span>");
        html_dom_set_style(ui->speed, "position", "absolute");
        html_dom_set_style(ui->speed, "pointer-events", "none");
        html_dom_set_style(ui->speed, "z-index", "4");
        html_dom_set_style(ui->speed, "white-space", "nowrap");
        html_dom_set_style(ui->speed, "font-family", "gamefont");
        html_dom_set_style(ui->speed, "transform", "translate(-50%, -50%)"); // textBaseline middle, centred
        html_dom_append(host, ui->speed);
        ui->speed_sig[0] = 0;
        ui->speed_cur = ui->speed_max = -1;
    }
    char sig[sizeof(ui->speed_sig)];
    snprintf(sig, sizeof(sig), "%g|%g|%g|%g|%s|%s", rs->speed_off_x, rs->speed_off_y, rs->speed_scale, rs->speed_opacity, rs->speed_color, rs->speed_color_max);
    if (strcmp(sig, ui->speed_sig) != 0) {
        char text[48];
        snprintf(text, sizeof(text), "%g%%", rs->speed_off_x * 10.0);
        html_dom_set_style(ui->speed, "left", text);
        snprintf(text, sizeof(text), "%g%%", rs->speed_off_y * 10.0);
        html_dom_set_style(ui->speed, "top", text);
        snprintf(text, sizeof(text), "%gpx", rs->speed_scale * 10.0);
        html_dom_set_style(ui->speed, "font-size", text);
        fmt_opacity(rs->speed_opacity, text, sizeof(text));
        html_dom_set_style(ui->speed, "opacity", text);
        dom_style("knSpeedCur", "color", rs->speed_color);
        dom_style("knSpeedMax", "color", rs->speed_color_max);
        snprintf(ui->speed_sig, sizeof(ui->speed_sig), "%s", sig);
    }
    if (!ui->speed_shown) {
        html_dom_set_display(ui->speed, "block");
        ui->speed_shown = 1;
    }
    if (now_ms < ui->speed_next_ms) return;
    ui->speed_next_ms = now_ms + SPEED_UPDATE_MS;

    const Player *me = client->me;
    int cur = 0;
    if (me && me->active && !client->game.move_lock) {
        cur = (int) lround(sqrt(me->velocity.x * me->velocity.x + me->velocity.y * me->velocity.y + me->velocity.z * me->velocity.z) * 1000.0);
    }
    const int max = cur > ui->speed_max ? cur : ui->speed_max;
    if (cur != ui->speed_cur) dom_text_int("knSpeedCur", cur);
    if (max != ui->speed_max) dom_text_int("knSpeedMax", max);
    ui->speed_cur = cur;
    ui->speed_max = max;
}

/* ------------------------------------------------------------ kill feed */

static void hud_feed_drop(const int index, const int remove_node) {
    HudUi *ui = &g_hud_ui;
    FeedRow *slot = &ui->feed[index];
    if (remove_node && slot->row) html_dom_remove(slot->row);
    memmove(slot, slot + 1, (size_t) (ui->feed_count - index - 1) * sizeof(*slot));
    ui->feed_count--;
}

static int hud_feed_find(const HtmlElem row) {
    for (int i = 0; i < g_hud_ui.feed_count; i++) {
        if (g_hud_ui.feed[i].row == row) return i;
    }
    return -1;
}

static void hud_feed_clear(const int remove_nodes) {
    while (g_hud_ui.feed_count > 0) hud_feed_drop(0, remove_nodes);
    g_hud_ui.feed_last_ms = -1.0;
}

// The game loop's bR pass: every row's life drops by the frame delta, clamped to
// config.dltMx like Krunker's, and the row is removed (elm.remove(), no fade) at 0.
// io_nukeUpdate(1): #imgFlash display block, opacity 1, no background image; the game loop then
// takes gameTimeDiff * 0.002 off the opacity each frame (gone after 500 ms) and hides it at 0.
#define NUKE_FLASH_FADE_PER_MS 0.002
static double g_nuke_flash; // the opacity, 0 = hidden

void hud_ui_nuke_flash(void) {
    const HtmlElem el = html_ui_available() ? html_dom_by_id("imgFlash") : 0;
    if (!el) return;
    html_dom_set_style(el, "background-image", "");
    html_dom_set_style(el, "opacity", "1");
    html_dom_set_display(el, "block");
    g_nuke_flash = 1.0;
}

void hud_ui_nuke_flash_step(const float delta) {
    if (g_nuke_flash <= 0.0) return;
    g_nuke_flash -= (double) delta * 1000.0 * NUKE_FLASH_FADE_PER_MS;
    const HtmlElem el = html_ui_available() ? html_dom_by_id("imgFlash") : 0;
    if (!el) return;
    if (g_nuke_flash <= 0.0) {
        g_nuke_flash = 0.0;
        html_dom_set_style(el, "opacity", "0");
        html_dom_set_display(el, "none");
        return;
    }
    char value[16];
    snprintf(value, sizeof(value), "%.3f", g_nuke_flash);
    html_dom_set_style(el, "opacity", value);
}

static void hud_feed_step(const double now_ms) {
    HudUi *ui = &g_hud_ui;
    double dt = ui->feed_last_ms >= 0.0 ? now_ms - ui->feed_last_ms : 0.0;
    ui->feed_last_ms = now_ms;
    if (dt <= 0.0) return;
    if (dt > FEED_DELTA_MAX_MS) dt = FEED_DELTA_MAX_MS;
    for (int i = 0; i < ui->feed_count;) {
        FeedRow *slot = &ui->feed[i];
        if (slot->life_ms > 0.0 && (slot->life_ms -= dt) <= 0.0) {
            hud_feed_drop(i, 1);
            continue;
        }
        i++;
    }
}

void hud_ui_add_killfeed(Client *client, const char *killer, const char *victim, const char *weapon_icon, const int is_headshot) {
    // TODO: "You"/ally coloring by uid/team once kill events carry them; name match for now.
    const char *you = hud_my_name(client);
    const int killer_is_you = killer && you[0] && strcmp(killer, you) == 0;
    const int victim_is_you = victim && you[0] && strcmp(victim, you) == 0;
    hud_ui_add_killfeed_ex(client, killer, killer_is_you, victim, victim_is_you, weapon_icon, is_headshot, -1, 0.0);
}

// Old Scoreboard (oldScoreboard -> switchLeaderboard(0), leaderType 0): gE() writes kill
// lines into #chatList instead of #killFeed.
static int hud_feed_in_chat(void) {
    return client_render_settings()->old_scoreboard;
}

// Evicts the oldest children of `list` (and their bR entries) until fewer than `limit` remain.
static void hud_feed_evict(const HtmlElem list, const int limit) {
    for (int guard = 0; guard < 256 && html_dom_child_count(list) >= limit; guard++) {
        const HtmlElem first = html_dom_first_child(list);
        if (!first) break;
        const int index = hud_feed_find(first);
        if (index >= 0) hud_feed_drop(index, 1);
        else html_dom_remove(first);
    }
}

// gE(): appends one row, `age_ms` into its life. #killFeed: feed_N, life 4 s, feedLimit
// rows; #chatList (Old Scoreboard): chatMsg_N, life chatTimer s (0 = no expiry),
// chatListLength rows shared with the chat lines, scrolled to the bottom.
static void hud_feed_insert(const int chat, const char *html, double age_ms) {
    HudUi *ui = &g_hud_ui;
    if (!html_ui_available() || !html_ui_is_ready()) return;
    const ClientRenderSettings *rs = client_render_settings();
    const double life = chat ? (double) rs->chat_timer * 1000.0 : FEED_LIFE_MS;
    if (age_ms < 0.0) age_ms = 0.0;
    if (life > 0.0 && age_ms >= life) return; // already gone

    const HtmlElem list = html_dom_by_id(chat ? "chatList" : "killFeed");
    if (!list) return;

    if (chat) {
        hud_feed_evict(list, CHAT_LIST_LENGTH);
    } else {
        // feedLimit: the oldest rows go first
        hud_feed_evict(list, CLAMP(rs->feed_limit, 1, FEED_MAX));
    }
    while (ui->feed_count >= FEED_MAX) hud_feed_drop(0, 1);

    char id[32];
    if (chat) snprintf(id, sizeof(id), "chatMsg_%d", ui->chat_seq++);
    else snprintf(id, sizeof(id), "feed_%d", ui->feed_seq++);

    const HtmlElem row = html_dom_create("div", id);
    html_dom_set_html(row, html);
    html_dom_append(list, row);
    if (chat) html_dom_scroll_to_bottom(list);

    FeedRow *slot = &ui->feed[ui->feed_count++];
    slot->row = row;
    slot->life_ms = life > 0.0 ? life - age_ms : 0.0;
    slot->chat = chat;
}

static void hud_h2h_count(const char *killer, int killer_is_you, const char *victim, int victim_is_you);

void hud_ui_add_killfeed_ex(Client *client, const char *killer, const int killer_is_you, const char *victim, const int victim_is_you,
                            const char *weapon_icon, const int is_headshot, const int streak, const double age_ms) {
    (void) client;
    hud_h2h_count(killer, killer_is_you, victim, victim_is_you); // player.killedBy (death screen)
    if (!client_render_settings()->show_kills) return; // SETTINGS.tmp.showKills
    const int chat = hud_feed_in_chat();
    char html[1024];
    hud_feed_row_html(chat, killer, killer_is_you, victim ? victim : "", victim_is_you, weapon_icon, is_headshot, streak, html, sizeof(html));
    hud_feed_insert(chat, html, age_ms);
}

void hud_ui_add_killfeed_suicide(Client *client, const char *victim, const int victim_is_you) {
    (void) client;
    if (!client_render_settings()->show_kills) return; // bf.tmp.showKillFeed
    const int chat = hud_feed_in_chat();
    char html[512];
    const int count = (int) (sizeof(SUICIDES) / sizeof(SUICIDES[0]));
    hud_feed_suicide_html(chat, victim ? victim : "", victim_is_you, SUICIDES[rand() % count], html, sizeof(html)); // aU.randInt(0, len - 1)
    hud_feed_insert(chat, html, 0.0);
}

// killStreakMessage (g3, "kst"): gB(null, t("player.killstreak", who, fixStr(streak)), 1), a
// plain game line (gy: chatItem + chatMsg, chatBGOp / chatTextOutline, chatListLength,
// chatTimer). who = "You're" (#fff) or the alias (scrambled in streamer mode, ally/enemy
// colour) + " is"; "{0} on a {1} Kill Streak".
void hud_ui_kill_streak(Client *client, const int uid, const int streak) {
    if (!html_ui_available() || !html_ui_is_ready()) return;
    Player *player = game_find_player(&client->game, uid);
    if (!player) return;
    char html[1024], who[512], num[16], line[768];
    size_t len = 0, wlen = 0;
    html[0] = who[0] = 0;
    hud_feed_open(html, sizeof(html), &len, 1);
    if (player == client->me) {
        hb_append(who, sizeof(who), &wlen, "<span style='color:" FEED_COLOR_YOU "'>You're</span>"); // v10: English in every language
    } else {
        const Player *me = client->me;
        const int ally = me && me->team && client->game.mode && client->game.mode->config.teams && me->team == player->team;
        hud_feed_name_span(who, sizeof(who), &wlen, hud_player_name(client, player), 0, ally);
        hb_append(who, sizeof(who), &wlen, " is");
    }
    snprintf(num, sizeof(num), "%d", streak);
    I18N_F(line, "player.killstreak", "{0} on a {1} Kill Streak", who, num);
    hb_append(html, sizeof(html), &len, "%s</span></div><br>", line);
    hud_feed_insert(1, html, 0.0);
}

/* ------------------------------------------------------------ chat */

// The chat channel of the local player (v7 vLN02023: 0 global, 1 team; #chatSwitch).
static int g_chat_channel;

// f2018's data-tab/display of a row on `channel` (-1 = every channel).
static void hud_chat_row_channel(const HtmlElem row, const int channel) {
    char tab[8];
    snprintf(tab, sizeof(tab), "%d", channel);
    html_dom_set_attr(row, "data-tab", tab);
    html_dom_set_display(row, channel == g_chat_channel || channel == -1 ? "" : "none");
}

static int hud_chat_teams(const Client *client) {
    return client && client->game.mode && client->game.mode->config.teams;
}

// The channel of an incoming player line: io_addChat forces -1 outside team modes. The v3 chat
// packet (older servers) has no channel, so its team-mode lines are global (0); v7 servers send
// NET_PACKET_CHAT_V7 with the channel (hud_ui_add_chat_channel).
static int hud_chat_line_channel(const Client *client, const int player_line) {
    return hud_chat_teams(client) && player_line ? 0 : -1;
}

// f2018: appends one #chatMsg_N row (see hud_chat_row_html for the three kinds) on `channel`;
// the oldest rows go first beyond config.chatListLength.
static void hud_add_chat_row_on(const char *name, const char *msg, const char *color, const int channel) {
    const HtmlElem list = html_dom_by_id("chatList");
    if (!list || !msg || !msg[0]) return;

    hud_feed_evict(list, CHAT_LIST_LENGTH);
    char id[32], html[1024];
    snprintf(id, sizeof(id), "chatMsg_%d", g_hud_ui.chat_seq++);
    hud_chat_row_html(name, msg, color, html, sizeof(html));

    const HtmlElem row = html_dom_create("div", id);
    hud_chat_row_channel(row, channel);
    html_dom_set_html(row, html);
    html_dom_append(list, row);
    html_dom_scroll_to_bottom(list);
}

static void hud_add_chat_row(const char *name, const char *msg, const char *color) {
    hud_add_chat_row_on(name, msg, color, hud_chat_line_channel(menu_ui()->client, name && name[0]));
}

// io_addChat(channel, ...) of a v7 "ch": the channel is forced to -1 outside team modes.
void hud_ui_add_chat_channel(const char *name, const char *msg, const char *color, const int channel) {
    if (!html_ui_available() || !html_ui_is_ready()) return;
    hud_add_chat_row_on(name, msg, color, hud_chat_teams(menu_ui()->client) ? channel : -1);
}

// index.js (per menu update): the team channel switches back to global when the mode has no teams
// (switchChat(chatSwitch) without its chat line). The comp lobby's team channel: hud_reset_page.
static void hud_chat_channel_check(const Client *client) {
    if (g_chat_channel != CHAT_CHANNEL_TEAM || hud_chat_teams(client)) return;
    g_chat_channel = CHAT_CHANNEL_GLOBAL;
    const HtmlElem sw = html_dom_by_id("chatSwitch");
    if (sw) html_dom_set_attr(sw, "data-tab", "public");
    const HtmlElem list = html_dom_by_id("chatList");
    for (HtmlElem row = list ? html_dom_first_child(list) : 0; row; row = html_dom_next(row)) {
        char ch[8] = "";
        html_dom_get_attr(row, "data-tab", ch, sizeof(ch));
        const int c = atoi(ch);
        html_dom_set_display(row, c == g_chat_channel || c == -1 ? "" : "none");
    }
}

// window.switchChat(#chatSwitch): toggles the text & voice channel (team modes only; forced
// to global elsewhere), shows the rows of that channel and says so in the chat.
static void hud_switch_chat(void) {
    const Client *client = menu_ui()->client;
    const HtmlElem sw = html_dom_by_id("chatSwitch");
    if (!sw) return;
    char tab[16] = "";
    html_dom_get_attr(sw, "data-tab", tab, sizeof(tab));
    int next = strcmp(tab, "public") == 0 ? 0 : 1; // the attribute names the channel after the switch
    const int teams = client && client->game.mode && client->game.mode->config.teams;
    if (!teams) next = 1;
    html_dom_set_attr(sw, "data-tab", next ? "public" : "groups");
    g_chat_channel = !next;
    const HtmlElem list = html_dom_by_id("chatList");
    for (HtmlElem row = list ? html_dom_first_child(list) : 0; row; row = html_dom_next(row)) {
        char ch[8] = "";
        html_dom_get_attr(row, "data-tab", ch, sizeof(ch));
        const int c = atoi(ch);
        html_dom_set_display(row, c == g_chat_channel || c == -1 ? "" : "none");
    }
    if (teams)
        hud_add_chat_row(NULL, g_chat_channel ? I18N("kn.chat.teamChannel", "Team Text & Voice Chat") : I18N("kn.chat.globalChannel", "Global Text & Voice Chat"),
                         SERVER_MSG_COLOR);
}

int hud_ui_chat_channel(void) { return g_chat_channel; }

// addChat(msg) for a server line.
static void hud_add_server_chat(const char *msg, const char *color) {
    hud_add_chat_row(NULL, msg, color);
}

void hud_ui_add_chat(const char *name, const char *msg, const char *color) {
    if (!html_ui_available() || !html_ui_is_ready()) return;
    hud_add_chat_row(name, msg, color);
}

// io_addChat(-1, null, html, 1): a system line whose message is markup (UTILS.fixStr only wraps it)
static void hud_add_chat_markup(const char *markup) {
    const HtmlElem list = html_dom_by_id("chatList");
    if (!list || !markup || !markup[0]) return;
    hud_feed_evict(list, CHAT_LIST_LENGTH);
    const ClientRenderSettings *rs = client_render_settings();
    char id[32], html[1024];
    size_t len = 0;
    html[0] = 0;
    snprintf(id, sizeof(id), "chatMsg_%d", g_hud_ui.chat_seq++);
    hb_append(html, sizeof(html), &len, "<div class='chatItem%s'", rs->chat_text_outline ? " chatTextOutline" : "");
    if (fabsf(rs->chat_bg_opacity - CHAT_BG_OPACITY_DEFAULT) > 1e-4f)
        hb_append(html, sizeof(html), &len, " style='background-color: rgba(0, 0, 0, %g)'", (double) rs->chat_bg_opacity);
    hb_append(html, sizeof(html), &len, "><span class='chatMsg'>" LRM "%s" LRM "</span></div><br>", markup);
    const HtmlElem row = html_dom_create("div", id);
    hud_chat_row_channel(row, -1);
    html_dom_set_html(row, html);
    html_dom_append(list, row);
    html_dom_scroll_to_bottom(list);
}

static int g_skin_reward_timer;
static void hud_skin_reward_hide(void *user) {
    (void) user;
    g_skin_reward_timer = 0;
    const HtmlElem el = html_dom_by_id("skinRewards");
    if (!el) return;
    html_dom_set_display(el, "none");
    html_dom_set_html(el, "");
}

// season-system: NET_PACKET_SKIN_FOUND. The finder: v7 io_skinReward (account.addSkin, the
// #skinRewards card for 5 s, "You found [name]", matchRewardSummary.skins); the others:
// io_skinFoundMessage ("<name> found [skin]" with Show Findings on).
void hud_ui_skin_found(Client *client, const int uid, const int skin_index) {
    if (!html_ui_available() || !html_ui_is_ready()) return;
    const ViewerSkin *skin = viewer_skin(skin_index);
    const Player *player = game_find_player(&client->game, uid);
    const int rarity = skin && skin->rarity >= 0 && skin->rarity < g_viewer_rarity_count ? skin->rarity : 0;
    const int rainbow = skin && skin->rarity == 6 && !client_render_settings()->disable_rarity_anim; // RENDER.rarityAnim && rarities[r].animate
    char name[256] = "", markup[768];
    size_t len = 0;
    if (skin) hb_append_escaped(name, sizeof(name), &len, skin->name ? skin->name : "");
    if (player && player == client->me) {
        account_add_skin(skin_index);
        hud_ui_add_reward_skin(skin_index);
        if (!skin) return;
        char thumb[256], card[1024];
        store_skin_preview(skin_index, thumb, sizeof(thumb));
        snprintf(card, sizeof(card), "<div class='skinRewCard' onclick='openSales(%d)' onmouseenter='playTick()'><img class='skinRewImg%s' src='%s' style='border: 3px solid %s' /><div class='skinRewRight'>%s<div class='skinRewDesc'>%s</div></div></div>",
                 skin_index, rainbow ? " rainbowBorder" : "", thumb, g_viewer_rarities[rarity].color, name, I18N("kn.store.itemFound", "Item Found!"));
        const HtmlElem el = html_dom_by_id("skinRewards");
        if (el) {
            html_dom_set_display(el, "block");
            html_dom_set_html(el, card);
            if (g_skin_reward_timer) html_timer_cancel(g_skin_reward_timer);
            g_skin_reward_timer = html_timer_add(5000.0, hud_skin_reward_hide, NULL);
        }
        char found[160];
        I18N_F(found, "kn.chat.youFound", "{0}You{1} found", "<span style='color:#fff'>", "</span>");
        snprintf(markup, sizeof(markup), "%s <span class='click%s' style='color:%s' onclick='viewItem(%d)'>[" LRM "%s" LRM "]</span>",
                 found, rainbow ? " rainbowText" : "", g_viewer_rarities[rarity].color, skin_index, name);
        hud_add_chat_markup(markup);
        return;
    }
    // v10.0.2 gates the line on Settings > Game > Chat "Show Findings" (showFindings; v7 used showUnboxings)
    if (!skin || !player || ui_setting_is_false("showFindings")) return;
    char who[PLAYER_NAME_SIZE * 2], shown[PLAYER_NAME_SIZE];
    size_t wlen = 0;
    who[0] = 0;
    hb_append_escaped(who, sizeof(who), &wlen, streamer_name(hud_player_name(client, player), shown, sizeof(shown)));
    // i18n "windows.store.found" = "{0}{1} found": "<span style='color:#fff'>" + fixStr(name) + "</span> found"
    char lrm_who[sizeof(who) + 8], found[sizeof(who) + 128];
    snprintf(lrm_who, sizeof(lrm_who), LRM "%s" LRM, who);
    I18N_F(found, "windows.store.found", "{0}{1} found", lrm_who, "</span>");
    snprintf(markup, sizeof(markup), "<span style='color:#fff'>%s <span class='click%s' style='color:%s' onclick='viewItem(%d)'>[" LRM "%s" LRM "]</span>",
             found, rainbow ? " rainbowText" : "", g_viewer_rarities[rarity].color, skin_index, name);
    hud_add_chat_markup(markup);
}

// v7 io_foundJunk's line: io_addChat(-1, null, "<span style='color:#fff'>You</span> found <span
// style='color:#cc51f8'>[" + amount + "]</span> Junk", 1). `line` is the server's plain
// "You found [x.xx] Junk"; anything else shows as a plain game line.
void hud_ui_found_junk(const char *line) {
    if (!html_ui_available() || !html_ui_is_ready() || !line) return;
    static const char prefix[] = "You found [";
    const char *amount = !strncmp(line, prefix, sizeof(prefix) - 1) ? line + sizeof(prefix) - 1 : NULL;
    const char *end = amount ? strchr(amount, ']') : NULL;
    if (!end || strcmp(end, "] Junk") != 0) {
        hud_ui_add_chat(NULL, line, NULL);
        return;
    }
    char raw[32], value[64], markup[256];
    size_t len = 0;
    value[0] = 0;
    snprintf(raw, sizeof(raw), "%.*s", (int) (end - amount), amount);
    hb_append_escaped(value, sizeof(value), &len, raw);
    I18N_F(markup, "kn.chat.foundJunk", "{0}You{1} found {2}[{3}]{4} Junk", "<span style='color:#fff'>", "</span>", "<span style='color:#cc51f8'>",
           value, "</span>");
    hud_add_chat_markup(markup);
}

// Empties #chatList and marks the session's join lines as written (a scenario brings
// its own chat history).
void hud_ui_clear_chat(void) {
    const HtmlElem list = html_dom_by_id("chatList");
    if (list) html_dom_set_html(list, "");
    for (int i = g_hud_ui.feed_count - 1; i >= 0; i--) {
        if (g_hud_ui.feed[i].chat) hud_feed_drop(i, 0); // their nodes went with the list
    }
    g_hud_ui.chat_seq = 0;
    g_hud_ui.chat_seeded = 1;
}

static void hud_seed_chat(const Client *client) {
    HudUi *ui = &g_hud_ui;
    if (ui->chat_seeded) return;
    // The join line names the player like the server does ("Guest_1"): wait for SPAWN's
    // name, then fall back to the menu name.
    if (client->me && !client->me->name[0] && ui->seed_wait++ < HUD_SEED_WAIT_SAMPLES) return;
    ui->chat_seeded = 1;
    ui->seed_wait = 0;

    hud_add_server_chat(hud_welcome_msg(client), SERVER_MSG_COLOR); // map welMsg
    const char *name = hud_my_name(client);
    if (name[0]) {                                                   // config.serverMessages[0]
        char msg[PLAYER_NAME_SIZE + sizeof(client->menu_name) + 32];
        snprintf(msg, sizeof(msg), "%s joined the game", name);
        hud_add_server_chat(msg, SERVER_MSG_COLOR);
    }
}

/* ------------------------------------------------------------ render groups */

static void hud_render_health(const HtmlHudState *hud) {
    char html[2048];
    HtmlElem el;

    // io_changeHealth: bottomLeftHealth.textContent = Math.ceil(hp)
    dom_text_int("bottomLeftHealth", hud->health < 0 ? 0 : hud->health);
    if ((el = html_dom_by_id("bottomLeftHealthBar"))) {
        hud_health_bar_html(hud->health, hud->max_health, hud->health_segments, html, sizeof(html));
        html_dom_set_html(el, html);
    }
}

static void hud_render_ammo(const HtmlHudState *hud) {
    char text[32];

    if (hud->ammo) snprintf(text, sizeof(text), "%d", hud->ammo);
    else snprintf(text, sizeof(text), "%s", hud->melee ? "-" : "0");
    dom_text("ammoVal", text);

    // "| " + (ammoLimit != null ? "-" : weapon.ammo || "-")
    if (hud->ammo_limit || hud->ammo_max <= 0) snprintf(text, sizeof(text), "| -");
    else snprintf(text, sizeof(text), "| %d", hud->ammo_max);
    dom_text("ammoMax", text);
    dom_display("ammoHolder", "inline-block");

    // reloadUIAnim: ammoHolder.style.backgroundSize = round(pct) + "%  100%", so the blank_2 fill
    // keeps the full box height and only its width drains; resetReloadUIAnim writes "0%".
    if (hud->reload_percent > 0) snprintf(text, sizeof(text), "%d%% 100%%", hud->reload_percent);
    else snprintf(text, sizeof(text), "0%%");
    dom_style("ammoHolder", "background-size", text);

    const int need_reload = !hud->melee && hud->ammo_max > 0 && hud->ammo <= 0 && !hud->ammo_limit;
    dom_display("reloadMsg", need_reload ? "block" : "none");
}

static void hud_render_timer(const char *timer_text) {
    dom_text("timerVal", timer_text);
}

// f1773: #bottomLeftClassImg = f1772(classIndex) = textures/classes/icon_<classIndex>.png
static void hud_render_class(const int class_index) {
    const HtmlElem img = html_dom_by_id("bottomLeftClassImg");
    if (!img) return;

    char src[64];
    snprintf(src, sizeof(src), "textures/classes/icon_%d.png", class_index < 0 ? 0 : class_index);
    html_dom_set_attr(img, "src", src);
}

// Team type / name of the player card (io_addPlayers / io_updateTeam, isYou): "FFA" without
// teams, else "Team" + the team's name in its mode colour.
static void hud_render_team(const Client *client) {
    const GameMode *mode = client->game.mode;
    const int teams = mode && (mode->config.teams || mode->config.team_options[0]);
    const int team = client->me ? client->me->team : 0;
    const char *name = "";
    if (teams && team >= 1 && team <= 2) name = mode->config.team_options[team - 1] ? mode->config.team_options[team - 1] : team == 2 ? "Bravo" : "Alpha";
    dom_text("bottomLeftTeamType", teams ? "Team" : "FFA");
    dom_text("bottomLeftTeamName", name);
    dom_style("bottomLeftTeamName", "color", teams && team >= 1 && team <= 2 && mode->config.team_colors[team - 1] ? mode->config.team_colors[team - 1] : "#fff");
}

void hud_ui_render_team(const Client *client) {
    hud_render_team(client);
}

static void hud_render_scope(const int scoped) {
    dom_style("aimRecticle", "opacity", scoped ? "1" : "0");
}

// players.js resetAim() (kill() -> toggleAim(player, 0)): the scope (#aimRecticle) and the
// sight reticle (#aimDot) go to opacity 0; the RENDER.zoom(1) half is game_events.c's.
// The HUD state follows so the next pass after a respawn only rewrites what changed.
static void hud_reset_aim(Client *client) {
    dom_style("aimRecticle", "opacity", "0");
    dom_style("aimDot", "opacity", "0");
    if (!client) return;
    client->hud_html.scoped = 0;
    client->hud_html.aim_dot = 0;
}

// f1777: pingText = ping == -1 ? "-" : ping, coloured by COLORS.ping.
static void hud_render_ping(const int ping) {
    char text[16];
    if (ping < 0) snprintf(text, sizeof(text), "-");
    else snprintf(text, sizeof(text), "%d", ping);
    dom_text("pingText", text);
    dom_style("pingText", "color", ping >= 150 ? PING_COLOR_HIGH : ping >= 50 ? PING_COLOR_MEDIUM : PING_COLOR_LOW);
}

// The weapon rack bottom-right: rebuilt when the list changes, only the items' opacities
// (and the melee icon after a throw) when the held slot changes.
static void hud_render_loadout(const HudUiExtra *extra, const int rebuild) {
    const HtmlElem host = html_dom_by_id("weapHolder");
    if (!host) return;

    if (rebuild) {
        g_combat_hash = 0; // the new .weapStreakHolder counters start at 0: hud_combat_step rewrites them
        static char html[32768]; // up to HUD_UI_LOADOUT_ALL slots
        const int base = extra->loadout_base > 0 && extra->loadout_base < extra->loadout_size ? extra->loadout_base : extra->loadout_size;
        hud_weapon_rack_html(extra->loadout, extra->skin, extra->loadout_size, base, extra->loadout_index, extra->can_throw, extra->melee_icon, html,
                             sizeof(html));
        html_dom_set_class(host, "knManyWeaps", extra->loadout_size > 4); // Object Manager: the compact rack
        html_dom_set_html(host, html);
        g_hud_ui.rack_can_throw = extra->can_throw;
        return;
    }
    const int throw_changed = extra->can_throw != g_hud_ui.rack_can_throw; // lastThrow != canThrow
    g_hud_ui.rack_can_throw = extra->can_throw;

    for (int i = 0; i < extra->loadout_size; i++) {
        char id[32];
        snprintf(id, sizeof(id), "weapItem_%d", i);
        const HtmlElem item = html_dom_by_id(id);
        if (!item) continue;
        html_dom_set_style(item, "opacity", i == extra->loadout_index ? "1" : "0.5");
        if (!throw_changed || !extra->loadout[i] || !extra->loadout[i]->melee) continue;
        const HtmlElem inner = html_dom_first_child(item); // .weapItemInner; the <img> is its last child
        HtmlElem img = inner ? html_dom_first_child(inner) : 0;
        for (HtmlElem next = img; next; next = html_dom_next(next)) img = next;
        if (img) {
            char src[256];
            html_dom_set_attr(img, "src", hud_weapon_icon_src(extra->loadout[i], extra->can_throw, extra->melee_icon, src, sizeof(src)));
        }
    }
}

// #matchInfo (top-left, next to the timer; v7 data/map.js): mode name, "on <map>".
static void hud_render_game_info(const Client *client) {
    const HtmlElem info = html_dom_by_id("matchInfo");
    const char *mode = hud_mode_name(client);
    if (!info || !mode[0]) return;

    char html[512];
    hud_game_info_html(mode, hud_map_name(client), html, sizeof(html));
    html_dom_set_html(info, html);
    hud_render_team(client);
}

// switchLeaderboard(old): Old Scoreboard (leaderType 0) shows #leaderboardHolder, whose
// #leaderContainer f1984 keeps filled with .leaderItem rows ("Empty Lobby" without players),
// and sets .topRightCounters to 400px wide (auto for leaderType 1); #killFeed shows only
// with the new board (and showKills).
static void hud_old_board_step(const Client *client) {
    HudUi *ui = &g_hud_ui;
    const ClientRenderSettings *rs = client_render_settings();
    const int old = rs->old_scoreboard;
    // v10 shows and fills #leaderboardHolder whatever the leaderType (hud_v10.c)
    const int board = 1;
    if (old != ui->old_board) {
        const HtmlElem counters = html_dom_query(0, ".topRightCounters"); // getElementsByClassName(..)[0]
        if (counters) html_dom_set_style(counters, "width", old ? "400px" : "auto");
        dom_display("leaderboardHolder", board ? "block" : "none");
        dom_display("killFeed", !old && rs->show_kills ? "inline-grid" : "none");
        ui->old_board = old;
        ui->old_board_hash = 0;
    }
    if (!board) return;
    char html[4096];
    hud_old_board_html(client, g_hud_override.scoreboard, html, sizeof(html));
    if (!html[0]) snprintf(html, sizeof(html), "%s", I18N("leaderboard.empty", "Empty Lobby"));
    const unsigned hash = hud_hash(html);
    if (hash == ui->old_board_hash) return;
    const HtmlElem c = html_dom_by_id("leaderContainer");
    if (c) html_dom_set_html(c, html);
    ui->old_board_hash = hash;
}

// f1987 / f1985: #centerLeaderDisplay is "inline-grid" while the scoreboard key is held;
// `html` is the hud_scoreboard_html() markup (ignored while hidden).
static void hud_render_scoreboard(const int shown, const char *html) {
    HudUi *ui = &g_hud_ui;
    const HtmlElem d = html_dom_by_id("centerLeaderDisplay");
    if (!d) return;

    if (shown) {
        const unsigned hash = hud_hash(html);
        if (ui->board_shown != 1 || hash != ui->board_hash) {
            html_dom_set_html(d, html);
            ui->board_hash = hash;
        }
    }
    if (shown != ui->board_shown) html_dom_set_display(d, shown ? "inline-grid" : "none");
    ui->board_shown = shown;
}

/* ------------------------------------------------------------ death screen / end screen */

static void hud_toggle_game_ui(int on);
static void hud_end_screen_dom(int start);
static void hud_combat_reset(void);
static double hud_now_ms(const Client *client);
static const Weapon *hud_weapon_by_name(const Game *game, const char *name);

static int g_end_result;        // io_gameEnd: 1 victory, 0 defeat, -1 draw
static char g_end_timer[32];    // "Next Round in <time>" (i18n timer.end)

// UI.toggleGameUI(j) (v7 src/libs/UI.js) while not spectating: the holders the death screen
// swaps out (j = false) and back (j = true).
static int g_hud_layout_epoch; // bumps on every pass that re-applies the HUD defaults (hud_v7modes.c follows)

static void hud_toggle_game_ui(const int on) {
    const char *block = on ? "block" : "none";
    g_hud_layout_epoch++;
    dom_display("spectateUI", "none");
    dom_display("mobileUI", on && ui_mobile_active() ? "block" : "none");
    dom_display("gameMessage", block);
    dom_display("roundMessage", block);
    dom_display("topLeftMain", on ? "inline-block" : "none");
    dom_display("topLeftMap", block);
    dom_display("topLeftMatchData", block);
    dom_display("topLeftData", on ? "inline-block" : "none");
    dom_display("topLeftBottom", block);
    dom_display("topRight", block);
    if (!on) dom_display("reloadMsg", "none"); // on: hud_render_ammo decides (valid = 0 rewrites it)
    dom_display("bottomLeftHolder", block);
    dom_display("weapHolder", on && !ui_mobile_active() ? "grid" : "none");
    dom_display("bottomRight", block);
    dom_display("deathUIHolder", on ? "none" : "block");
    dom_style("speakerDisplay", "right", on ? "380px" : "20px");
    dom_display("overlay", block);
    dom_display("vignette", block);
    dom_display("interactMsg", "none");
    // (.onGame is toggleControlUI's, not toggleGameUI's: hud_apply_visibility and its cache own it)
    // Krunker's crosshair and hit marker live on the overlay canvas hidden above.
    if (!on) {
        hud_hide_crosshair();
        hud_hide_hit();
        hud_dmg_hide_all();
    }
}

// Head-to-head counts of the death screen (player.killedBy[id], counted from the kill feed
// events: v7 counts them in io_killPlayer the same way) and the life the stats describe.
#define H2H_MAX 64
static struct {
    char name[PLAYER_NAME_SIZE];
    int they_killed_me, i_killed_them;
} g_h2h[H2H_MAX];
static int g_h2h_count;
static double g_life_start_ms = -1.0; // HUD clock of the spawn (Time Survived)
static int g_life_kills, g_life_score_start;

static int hud_h2h_find(const char *name, const int create) {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < g_h2h_count; i++) {
        if (!strcmp(g_h2h[i].name, name)) return i;
    }
    if (!create || g_h2h_count >= H2H_MAX) return -1;
    snprintf(g_h2h[g_h2h_count].name, sizeof(g_h2h[0].name), "%s", name);
    g_h2h[g_h2h_count].they_killed_me = g_h2h[g_h2h_count].i_killed_them = 0;
    return g_h2h_count++;
}

// Kills by the local player (kill feed events); our deaths count in hud_ui_show_death, where
// io_killPlayer does `player.killedBy[otherPlayer.id]++` right before f1975.
static void hud_h2h_count(const char *killer, const int killer_is_you, const char *victim, const int victim_is_you) {
    (void) killer;
    if (killer_is_you && !victim_is_you) {
        const int i = hud_h2h_find(victim, 1);
        if (i >= 0) g_h2h[i].i_killed_them++;
        g_life_kills++;
    }
}

// f1974: the death screen's weapon art, img/death-ui/<weaponConfig.icon>.png (melee: icon_0_1,
// fists: icon_0).
static const char *hud_death_weapon_img(const Weapon *w, const char *icon, char *out, const size_t size) {
    if ((w && w->melee) || (icon && !strcmp(icon, FEED_MELEE_ICON))) snprintf(out, size, "./img/death-ui/icon_0_1.png");
    else snprintf(out, size, "./img/death-ui/%s.png", w && w->icon ? w->icon : icon && icon[0] ? icon : "icon_0");
    return out;
}

// UTILS.getReadableTime(ms, true, null, true): "<m>m <s>s " (zero parts dropped).
static void hud_readable_time(const int ms, char *out, const size_t size) {
    const int secs = ms / 1000, m = secs / 60 % 60, h = secs / 3600;
    size_t len = 0;
    out[0] = 0;
    if (h) hb_append(out, size, &len, "%dh ", h);
    if (m) hb_append(out, size, &len, "%dm ", m);
    if (secs % 60) hb_append(out, size, &len, "%ds ", secs % 60);
}

// f1975: the death screen (#deathUIHolder). With a killer: the VS layout, the killer's card
// left (weapon [skin] name in its rarity colour and art, his kills on us, the player card
// banner, class icon, "lvl N" from the "3" class stat, alias and the picked player card stat)
// and ours right (banner, class level of our account, name), "[key] to Report" in between;
// without one "You Died". Below, the life's stats (Time Survived, Shots Fired, Accuracy, Kills,
// Score) from NET_PACKET_DEATH_INFO; a value the server did not send shows v7's "Unknown".
// Not ported: the rainbowText class of animated rarities (KrunkNative's store table has no
// rarity animate flag).
static void hud_death_banner(char *out, const size_t size, size_t *len, const char *url) {
    if (!url || !url[0]) return;
    hb_append(out, size, len, "<img class=\"death-row-bottom-bg\" src=\"");
    hb_append_escaped(out, size, len, url);
    hb_append(out, size, len, "\">");
}

static void hud_death_html(const Client *client, const HudKillCard *card, char *out, const size_t size) {
    size_t len = 0;
    out[0] = 0;
    char stats[1024];
    size_t slen = 0;
    stats[0] = 0;
    if (card && card->stats) {
        static const char *const names[5] = {"Time Survived", "Shots Fired", "Accuracy", "Kills", "Score"};
        char vals[5][32];
        hud_readable_time(card->time_alive_ms, vals[0], sizeof(vals[0]));
        snprintf(vals[1], sizeof(vals[1]), "%s", card->shots >= 0 ? "" : "Unknown");
        if (card->shots >= 0) snprintf(vals[1], sizeof(vals[1]), "%d", card->shots);
        snprintf(vals[2], sizeof(vals[2]), "%s", card->accuracy && card->accuracy[0] ? card->accuracy : "Unknown");
        snprintf(vals[3], sizeof(vals[3]), "%d", card->kills);
        snprintf(vals[4], sizeof(vals[4]), "%d", card->life_score);
        for (int i = 0; i < 5; i++) {
            hb_append(stats, sizeof(stats), &slen, "<div class=\"death-stats-row\">\n\t\t\t%s : <span style=\"color:rgba(255, 255, 255, 0.7)\">",
                      names[i]);
            hb_append_escaped(stats, sizeof(stats), &slen, i == 0 && !vals[0][0] ? "Unknown" : vals[i]);
            hb_append(stats, sizeof(stats), &slen, "</span>\n\t\t</div>");
        }
    }
    const int by = card && card->killer && card->killer[0];
    if (!by) {
        hb_append(out, size, &len, "<div class=\"death-ui-bottom-empty\">\n\t\t\t<div class=\"death-column-empty\"></div>\n\t\t\t<div class=\"death-column\" style=\"justify-content: center;\">"
                                   "\n\t\t\t\t<div class=\"death-row-center-self\">You</div>\n\t\t\t\t<div class=\"death-row-center-self\">Died</div>\n\t\t\t</div>"
                                   "\n\t\t\t<div class=\"death-stats\">%s</div>\n\t\t</div>", stats);
        return;
    }
    const Player *me = client->me;
    const Weapon *my_weapon = me ? me->weapon : NULL;
    char killer_img[96], my_img[96], shown[PLAYER_NAME_SIZE];
    hud_death_weapon_img(NULL, card->weapon_icon, killer_img, sizeof(killer_img));
    hud_death_weapon_img(my_weapon, NULL, my_img, sizeof(my_img));
    const int h2h = hud_h2h_find(card->killer, 0);
    const int my_class = me ? me->class_index : client->menu_class_index;
    const char *color = card->weapon_color && card->weapon_color[0] ? card->weapon_color : "rgba(255,255,255,0.7)";
    hb_append(out, size, &len, "\n\t\t\t<div class=\"death-ui-bottom\" id='deathUIBottom'>\n\t\t\t\t<div class=\"death-column-empty\"></div>"
                               "\n\t\t\t\t<div class=\"death-column\" style=\"justify-content: end;\">\n\t\t\t\t\t<div class=\"death-row-left\">"
                               "\n\t\t\t\t\t\t<div class=\"death-row-weapon\">\n\t\t\t\t\t\t\t<div class=\"death-row-weapon-text\" style=\"color:");
    hb_append_escaped(out, size, &len, color);
    hb_append(out, size, &len, "\">%s[", card->thrown ? "Thrown " : "");
    hb_append_escaped(out, size, &len, card->weapon ? card->weapon : "");
    hb_append(out, size, &len, "]</div>\n\t\t\t\t\t\t\t<div class=\"death-row-weapon-img\" style=\"background-image:url(%s)\"></div>\n\t\t\t\t\t\t</div>"
                               "\n\t\t\t\t\t\t<div class=\"death-row-kills\">%d</div>\n\t\t\t\t\t</div>\n\t\t\t\t\t<div class=\"death-row-bottom\">\n\t\t\t\t\t\t",
              killer_img, h2h >= 0 ? g_h2h[h2h].they_killed_me : 0);
    hud_death_banner(out, size, &len, card->killer_card_url);
    hb_append(out, size, &len, "\n\t\t\t\t\t\t<div class=\"death-row-class\">\n\t\t\t\t\t\t\t<img class=\"death-row-class-img\" src=\"textures/classes/icon_%d.png\">"
                               "\n\t\t\t\t\t\t\t<div class=\"death-row-class-level\">lvl %d</div>\n\t\t\t\t\t\t</div>\n\t\t\t\t\t\t<div class=\"death-row-user\">"
                               "\n\t\t\t\t\t\t\t<div class=\"death-row-user-text\">",
              card->class_index < 0 ? 0 : card->class_index, card->level > 1 ? card->level : 1);
    hb_append_escaped(out, size, &len, streamer_name(card->killer, shown, sizeof(shown))); // streamMode ? scrambleS(alias)
    hb_append(out, size, &len, "</div>\n\t\t\t\t\t\t\t");
    if (card->pc_stat_name && card->pc_stat_name[0]) { // "3" [2]: [pcStatIndex, value]
        hb_append(out, size, &len, "<div class=\"death-row-user-stat\">\n\t\t\t\t\t\t\t\t");
        hb_append_escaped(out, size, &len, card->pc_stat_value ? card->pc_stat_value : "");
        hb_append(out, size, &len, " <span style=\"color:#fff\">");
        hb_append_escaped(out, size, &len, card->pc_stat_name);
        hb_append(out, size, &len, "</span>\n\t\t\t\t\t\t\t</div>");
    }
    hb_append(out, size, &len, "\n\t\t\t\t\t\t</div>\n\t\t\t\t\t</div>\n\t\t\t\t</div>"
                               "\n\t\t\t\t<div class=\"death-column\" style=\"justify-content: center;\">\n\t\t\t\t\t<div class=\"death-row-center\">-</div>"
                               "\n\t\t\t\t\t<div class=\"death-row-center death-row-center-bottom\">VS</div>\n\t\t\t\t</div>"
                               "\n\t\t\t\t<div class=\"death-column\" style=\"justify-content: start;\">\n\t\t\t\t\t");
    if (card->report) { // "[" + UTILS.getKeyName(CONTROLS.binds.playerList.val) + "] to Report"
        char key[32];
        hb_append(out, size, &len, "<div class=\"death-report\">\n\t\t\t\t\t\t<span class=\"death-report-text\">[");
        hb_append_escaped(out, size, &len, ui_controls_key_name(ui_controls_bind(UI_CONTROL_PLAYER_LIST), key, sizeof(key)));
        hb_append(out, size, &len, "]</span> to Report\n\t\t\t\t\t</div>");
    }
    hb_append(out, size, &len, "\n\t\t\t\t\t<div class=\"death-row-right\">"
                               "\n\t\t\t\t\t\t<div class=\"death-row-kills\">%d</div>\n\t\t\t\t\t\t<div class=\"death-row-weapon\">"
                               "\n\t\t\t\t\t\t\t<div class=\"death-row-weapon-text\" style=\"color:rgba(255,255,255,0.7)\">[",
              h2h >= 0 ? g_h2h[h2h].i_killed_them : 0);
    hb_append_escaped(out, size, &len, my_weapon && my_weapon->name ? my_weapon->name : "");
    const AccountInfo *acc = account_get();
    hb_append(out, size, &len, "]</div>\n\t\t\t\t\t\t\t<div class=\"death-row-weapon-img-right\" style=\"background-image:url(%s)\"></div>\n\t\t\t\t\t\t</div>"
                               "\n\t\t\t\t\t</div>\n\t\t\t\t\t<div class=\"death-row-bottom\">\n\t\t\t\t\t\t", my_img);
    hud_death_banner(out, size, &len, card->my_card_url);
    hb_append(out, size, &len, "\n\t\t\t\t\t\t<div class=\"death-row-class\">"
                               "\n\t\t\t\t\t\t\t<img class=\"death-row-class-img\" src=\"textures/classes/icon_%d.png\">\n\t\t\t\t\t\t\t<div class=\"death-row-class-level\">lvl %d</div>"
                               "\n\t\t\t\t\t\t</div>\n\t\t\t\t\t\t<div class=\"death-row-user\">\n\t\t\t\t\t\t\t<div class=\"death-row-user-text\">",
              my_class < 0 ? 0 : my_class, card->my_level > 1 ? card->my_level : 1);
    hb_append_escaped(out, size, &len, acc ? (acc->premium && acc->alias[0] ? acc->alias : acc->name) : hud_my_name(client));
    hb_append(out, size, &len, "</div>\n\t\t\t\t\t\t</div>\n\t\t\t\t\t</div>\n\t\t\t\t</div>\n\t\t\t\t<div class=\"death-stats\">%s</div>\n\t\t\t</div>\n\t\t", stats);
}

// io_killPlayer for the local player: f1975's #deathUIHolder, uiBase.onDeathScrn and
// UI.toggleGameUI(false). `card` NULL (or no killer) is the "You Died" variant.
void hud_ui_show_death(Client *client, const HudKillCard *card) {
    if (!html_ui_available() || !html_ui_is_ready()) return;
    hud_reset_aim(client); // kill(): toggleAim(player, 0) for the local player
    HudKillCard local = card ? *card : (HudKillCard) {0};
    if (local.killer && local.killer[0]) { // player.killedBy[killer.id]++
        const int i = hud_h2h_find(local.killer, 1);
        if (i >= 0) g_h2h[i].they_killed_me++;
    }
    if (!local.weapon_icon && local.weapon) { // the killer's weapon art from its name
        const Weapon *w = hud_weapon_by_name(&client->game, local.weapon);
        if (w) local.weapon_icon = w->melee ? FEED_MELEE_ICON : w->icon;
    }
    if (!local.stats && g_life_start_ms >= 0.0) { // the life the stats describe, sampled here (the kill packet has no stats array yet)
        local.stats = 1;
        local.time_alive_ms = (int) (hud_now_ms(client) - g_life_start_ms);
        local.shots = -1;
        local.kills = g_life_kills;
        local.life_score = client->me ? client->me->score - g_life_score_start : 0;
    }
    char *html = malloc(8192);
    if (html) {
        hud_death_html(client, &local, html, 8192);
        const HtmlElem holder = html_dom_by_id("deathUIHolder");
        if (holder) html_dom_set_html(holder, html);
        free(html);
    }
    dom_display("reloadMsg", "none");
    hud_combat_reset();             // perkHolder.innerHTML = ""
    dom_text("zPerksHolder", "");
    const HtmlElem base = html_dom_by_id("uiBase");
    if (base) html_dom_set_class(base, "onDeathScrn", 1);
    g_hud_ui.dead = 1;
    hud_toggle_game_ui(0);
    dom_display("deathUIHolder", "block");
}

// Respawn: toggleGameUI(true) (toggleMenuHider drops onDeathScrn).
void hud_ui_hide_death(Client *client) {
    if (!g_hud_ui.dead) return;
    g_hud_ui.dead = 0;
    hud_dmg_clear();
    g_hud_ui.speed_max = 0;   // aV.Qp9.max = 0
    g_hud_ui.speed_cur = -1;
    const HtmlElem base = html_dom_by_id("uiBase");
    if (base) html_dom_set_class(base, "onDeathScrn", 0);
    hud_toggle_game_ui(1);
    client->hud_html.valid = 0; // rewrite every value group (and the crosshair)
    g_hud_ui.cross_shown = -1;
}

// UI.showEndScreen(false) -> UI.hideUI(false) (incl. toggleMenu(false)), plus the
// results' #endInfo once they show. `start` (the END): showEndScreen's resets of the results'
// parts (#endMidHolder, the vote cards, the team totals, #endInfo), which the results
// (hud_end_results) fill and show later. Without it (the menu closed over the end screen:
// UI.toggleEndScreen) only #endUI and the page around it come back, like v7, whose
// toggleMenu(true) hides #endUI alone and leaves the results' parts as they were.
static void hud_end_screen_dom(const int start) {
    if (start) { // showEndScreen(false)
        dom_display("endMidHolder", "none");
        dom_display("matchVoteHolder", "none");
        dom_display("teamTotal0", "none");
        dom_display("teamTotal1", "none");
        dom_display("endInfo", "none");
        dom_display("matchEndSignupIncentives", "none"); // v10.0.2 showEndScreen
    }
    dom_display("endUI", "");
    const HtmlElem base = html_dom_by_id("uiBase");
    if (base) {
        html_dom_set_class(base, "onEndScrn", 1);
        // hideUI(false) -> toggleMenu(false) -> toggleMenuHider(false)
        html_dom_set_class(base, "onMenu", 0);
        html_dom_set_class(base, "onDeathScrn", 0);
    }
    dom_display("menuHolder", "none");
    dom_display("speakerDisplay", "block");
    dom_display("menuHider", "none");
    dom_display("gameNameHolder", "none");
    dom_display("spectButton", "none");
    html_dom_set_class(html_dom_root(), "kn-hud-visible", 0); // its !important rule would keep #inGameUI up (ui/css/hud.css)
    dom_display("inGameUI", "none");
    dom_display("customGUIHolder", "none");
    dom_display("overlay", "none");
    dom_display("vignette", "none");
    dom_display("spectateUI", "none");
    dom_display("aimRecticle", "none");
    dom_display("instructionHolder", "none");
    dom_display("baseLinks", "none");
    dom_style("speakerDisplay", "right", "20px");
    dom_display("purchaseHolder", "none");
    dom_display("consentBlock", "none");
    // io_gameEnd: victorySub.src = "./img/" + (vic|def|dra) + ".png"; shown by f1913 after the
    // end animation (the animated title itself is overlay.vicTitle, #knVicTitle here)
    const HtmlElem sub = html_dom_by_id("victorySub");
    if (sub) {
        html_dom_set_attr(sub, "src", g_end_result < 0 ? "./img/dra.png" : g_end_result ? "./img/vic.png" : "./img/def.png");
        html_dom_set_display(sub, g_hud_ui.end_phase == 2 ? "" : "none");
    }
    if (g_hud_ui.end_phase == 2) dom_display("endInfo", "block");
    if (g_end_timer[0]) {
        char text[128];
        I18N_F(text, "timer.end", "Next Round in {0}", g_end_timer);
        dom_text("endTimer", text);
    }
    hud_hide_crosshair();
    hud_hide_hit();
    hud_dmg_hide_all();
}

static double hud_now_ms(const Client *client) {
    if (g_hud_clock_s >= 0.0) return g_hud_clock_s * 1000.0;
    return client && client->window ? glfwGetTime() * 1000.0 : 0.0;
}

double hud_ui_now_ms(const Client *client) { return hud_now_ms(client); }

int hud_ui_project(const Client *client, const float x, const float y, const float z, double *sx, double *sy) {
    return hud_project(client, x, y, z, sx, sy);
}

// #knVicTitle: overlay.vicTitle on the #game-overlay canvas (below #uiBase): a full-screen
// rgba(0,0,0,.33) fill and img/<vic|def|dra>.png centred at its natural size x scale, both
// at globalAlpha = the title opacity. Created next to #game-overlay so it stacks like the
// canvas, at page ready: game-overlay.js loads the three images at startup and only draws one
// once it isReady, so on the web they must not start loading at the END (behind a CDN round
// trip the whole zoom ran before the image arrived). One <img> per type, the shown one by display.
static const char *const END_TITLE_TYPES[3] = {"vic", "def", "dra"};

static HtmlElem hud_end_title(void) {
    HudUi *ui = &g_hud_ui;
    if (ui->end_title) return ui->end_title;
    HtmlElem el = html_dom_by_id("knVicTitle");
    if (!el) {
        const HtmlElem canvas = html_dom_by_id("game-overlay");
        if (!canvas) return 0;
        char html[1024];
        size_t len = 0;
        hb_append(html, sizeof(html), &len, "<div id='knVicTitle' style='position:absolute;left:0;top:0;width:100%%;height:100%%;z-index:1;"
                                            "pointer-events:none;background-color:rgba(0,0,0,0.33);display:none'>");
        for (int i = 0; i < 3; i++) {
            hb_append(html, sizeof(html), &len, "<img id='knVicTitle_%s' src='./img/%s.png' style='position:absolute;left:50%%;top:50%%;"
                                                "transform:translate(-50%%,-50%%);display:none'>", END_TITLE_TYPES[i], END_TITLE_TYPES[i]);
        }
        hb_append(html, sizeof(html), &len, "</div>");
        html_dom_insert_html(canvas, HTML_INSERT_AFTER_END, html);
        el = html_dom_by_id("knVicTitle");
    }
    ui->end_title = el;
    ui->end_title_key[0] = 0;
    return el;
}

static void hud_end_title_hide(void) {
    HudUi *ui = &g_hud_ui;
    if (ui->end_title) html_dom_set_display(ui->end_title, "none");
    ui->end_title_key[0] = 0;
}

// One overlay.render frame of the title (vic_title.c): `show` = #endUI up with vicTitle.show,
// `dt_ms` = the frame's gameTimeDiff. Hidden: v7's reset branch, and the layer goes away.
static void hud_end_title_frame(const int show, const double dt_ms) {
    HudUi *ui = &g_hud_ui;
    double opacity = 0.0, scale = 1.0;
    if (!vic_title_frame(&ui->vic, show, dt_ms, &opacity, &scale)) {
        if (ui->end_title_key[0]) hud_end_title_hide();
        return;
    }
    const HtmlElem title = hud_end_title();
    if (!title) return;

    // overlay px -> CSS px: the canvas is scaled by c7 = innerHeight / #uiBase height
    float vw = 0.0f, vh = 0.0f, dpr = 1.0f;
    html_dom_viewport(&vw, &vh, &dpr);
    const float base_h = hud_ui_height();
    const double c7 = base_h > 0.0f && vh > 0.0f ? vh / base_h : 1.0;
    const char *type_name = vic_title_type(g_end_result < 0 ? -1 : g_end_result); // io_gameEnd: vicTitle.type
    int type = 0;
    while (type < 2 && strcmp(END_TITLE_TYPES[type], type_name)) type++;
    char key[64], text[48];
    snprintf(key, sizeof(key), "%.3f|%.3f|%.4f|%d", opacity, scale, c7, type);
    if (!strcmp(key, ui->end_title_key)) return;
    snprintf(ui->end_title_key, sizeof(ui->end_title_key), "%s", key);

    for (int i = 0; i < 3; i++) {
        char id[32];
        snprintf(id, sizeof(id), "knVicTitle_%s", END_TITLE_TYPES[i]);
        const HtmlElem img = html_dom_by_id(id);
        if (!img) continue;
        html_dom_set_display(img, i == type ? "block" : "none");
        if (i != type) continue;
        snprintf(text, sizeof(text), "%.2fpx", END_TITLE_W * scale * c7);
        html_dom_set_style(img, "width", text);
        snprintf(text, sizeof(text), "%.2fpx", END_TITLE_H * scale * c7);
        html_dom_set_style(img, "height", text);
    }
    fmt_opacity(opacity, text, sizeof(text));
    html_dom_set_style(title, "opacity", text);
    html_dom_set_display(title, "block");
}

// GUI.endScreen (v7 src/ui/game.js): tab ids/names in their order.
#define END_TAB_COUNT 4
static const char *const END_TABS[END_TAB_COUNT][2] = {{"leaderboard", "Leaderboard"}, {"performance", "Performance"}, {"medals", "Medals"}, {"rewards", "Rewards"}};
static int g_end_tab;                 // GUI.endScreen.currentTab (index into END_TABS)
static const Client *g_end_client;    // the client the results describe (tab switches re-render)

// config.endForm.reward (applied by the server to its endData.ed cell): "<span
// style='color:#F8C55C'>+N</span> KR", "NONE" without one; also the performance tab's funds.
static const char *hud_reward_html(const int reward, char *out, const size_t size) {
    if (reward) snprintf(out, size, "<span style='color:#F8C55C'>+%d</span> KR", reward);
    else snprintf(out, size, "NONE");
    return out;
}

// f1917's row of #endTable (config.endStats: name, score, kills, deaths, reward): rank, the
// pfp + profile link (local #fff, team mate rgba(255,255,255,0.6), else COLORS.teams[1]),
// the stats and the empty actions cell (guests: no share / report buttons).
static void hud_end_row_html(char *out, const size_t size, size_t *len, const int rank, const int last, const char *name,
                             const char *color, const int score, const int kills, const int deaths, const int reward) {
    char shown[PLAYER_NAME_SIZE];
    hb_append(out, size, len, "<tr style='%s'><td style='color:#fff'>%d.</td><td style='position: relative;'>", last ? "border-bottom:none" : "", rank);
    hb_append(out, size, len, "<img class='endTablePfp' onerror='this.onerror=null;this.src=\"./img/player_pfp.png\";'src='./img/player_pfp.png'>");
    hb_append(out, size, len, "<a class='endTableN' href='./social.html?p=profile&q=");
    hb_append_escaped(out, size, len, name);
    hb_append(out, size, len, "' target='_blank' style='color:%s'>", color);
    hb_append_escaped(out, size, len, streamer_name(name, shown, sizeof(shown))); // streamMode ? scrambleS(alias)
    char cell[96];
    hb_append(out, size, len, "</a></td><td style='position: relative;'>%d</td><td style='position: relative;'>%d</td><td style='position: relative;'>%d</td>"
                              "<td style='position: relative;'>%s</td>", score, kills, deaths, hud_reward_html(reward, cell, sizeof(cell)));
    hb_append(out, size, len, "<td style='width: 1px;white-space: nowrap;padding-right: 0px;padding-left: 0px;'></td></tr>");
}

// Scenario rows (state.scoreboard [{name, score, kills?, deaths?, reward?, me?}]): FFA, so
// every row but ours gets COLORS.teams[1] like a live FFA table.
static int hud_end_scenario_rows_html(const Client *client, const cJSON *rows, char *out, const size_t size, size_t *len) {
    const int count = cJSON_GetArraySize(rows);
    const char *my_name = hud_my_name(client);
    int index = 0;
    const cJSON *row;
    cJSON_ArrayForEach(row, rows) {
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(row, "name");
        const cJSON *me = cJSON_GetObjectItemCaseSensitive(row, "me");
        const char *name = cJSON_IsString(n) ? n->valuestring : "";
        const int is_you = cJSON_IsBool(me) ? cJSON_IsTrue(me) : count == 1 || (my_name[0] && !strcmp(name, my_name));
#define ROW_INT(key) (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(row, key)) ? cJSON_GetObjectItemCaseSensitive(row, key)->valueint : 0)
        index++;
        hud_end_row_html(out, size, len, index, index >= count, name, is_you ? "#fff" : FEED_COLOR_ENEMY,
                         ROW_INT("score"), ROW_INT("kills"), ROW_INT("deaths"), ROW_INT("reward"));
#undef ROW_INT
    }
    return count;
}

// tabs.leaderboard: <table id="endTable"> filled like f1917 (endStats sid, name, score,
// kills, deaths, reward).
static void hud_end_leaderboard_html(const Client *client, char *out, const size_t size, size_t *len) {
    hb_append(out, size, len, "<table id=\"endTable\"><tr><th id='endlb_rank'>Rank</th><th id='endlb_name'>Name</th><th id='endlb_score'>Score</th>"
                              "<th id='endlb_kills'>Kills</th><th id='endlb_deaths'>Deaths</th><th id='endlb_reward'>Reward</th><th></th></tr><tr height='20px;'></tr>");
    if (cJSON_IsArray(g_hud_override.scoreboard) && hud_end_scenario_rows_html(client, g_hud_override.scoreboard, out, size, len)) {
        hb_append(out, size, len, "</table>");
        return;
    }

    const Player *players[HUD_BOARD_PLAYERS_MAX];
    int count = client->game.players ? hud_sorted_players(client, players, HUD_BOARD_PLAYERS_MAX) : 0;
    if (count == 0 && client->me) {
        players[0] = client->me;
        count = 1;
    }
    for (int i = 0; i < count; i++) {
        const Player *p = players[i];
        // localTeam = player.team || (spectating ? 1 : 0); a teamless (FFA) player's team is
        // null there, so `0 == null` makes every other FFA row red.
        const int local_team = client->me && client->me->team ? client->me->team : spectate_active() ? 1 : 0;
        const char *color = p == client->me ? "#fff" : p->team && p->team == local_team ? "rgba(255,255,255,0.6)" : FEED_COLOR_ENEMY;
        hud_end_row_html(out, size, len, i + 1, i + 1 >= count, hud_player_name(client, p), color, p->score, (int) p->kills,
                         (int) p->deaths, p->reward);
    }
    hb_append(out, size, len, "</table>");
}

// tabs.performance.gen() for a guest (no account: no battle pass / XP bar; W/L from this
// match). stats = f1914: rank "#N", score, kills, deaths, funds (the reward, "NONE" without).
static void hud_end_performance_html(const Client *client, char *out, const size_t size, size_t *len) {
    const Player *players[HUD_BOARD_PLAYERS_MAX];
    const int count = client->game.players ? hud_sorted_players(client, players, HUD_BOARD_PLAYERS_MAX) : 0;
    int rank = 1;
    for (int i = 0; i < count; i++) {
        if (players[i] == client->me) rank = i + 1;
    }
    const Player *me = client->me;
    const int score = me ? me->score : 0;
    const int kills = me ? (int) me->kills : 0;
    const int deaths = me ? (int) me->deaths : 0;
    // v7.1.4 Battle Pass: #endBPLayout (account && !isCustom && bpData; battlepass_ui.c)
    StrBuf bp = {0};
    if (battlepass_ui_end_html(&bp)) {
        char *html = sb_finish(&bp);
        hb_append(out, size, len, "%s", html);
        free(html);
    } else {
        sb_free(&bp);
    }
    // v10: Account Level, Combat Earnings + Combat Record (hud_v10.c)
    char kr[96];
    hud_v10_end_performance_html(out, size, len, hud_reward_html(me ? me->reward : 0, kr, sizeof(kr)), score, kills, deaths, rank);
}

// ---- WP-E part 2c: the Medals and Rewards tabs, the completed challenge cards

static NetEndMedalsPacket g_end_medals; // endData.mdls of this end (NET_PACKET_END_MEDALS)
static void hud_end_card_render(const Client *client);
#define END_REWARD_SKINS_MAX 16
static int g_end_skins[END_REWARD_SKINS_MAX][2]; // matchRewardSummary.skins [{i, c}]
static int g_end_skin_count;

void hud_ui_set_end_medals(const NetEndMedalsPacket *packet) {
    if (packet) g_end_medals = *packet;
    if (g_end_medals.count > PLAYER_MEDAL_SLOTS) g_end_medals.count = PLAYER_MEDAL_SLOTS;
    if (g_hud_ui.ended && g_hud_ui.end_phase == 2 && g_end_tab == 2 && g_end_client) hud_end_card_render(g_end_client);
}

// io_challengeCompleted: a challenge's skins go to matchRewardSummary.skins (counted)
void hud_ui_add_reward_skin(const int skin) {
    for (int i = 0; i < g_end_skin_count; i++) {
        if (g_end_skins[i][0] == skin) {
            g_end_skins[i][1]++;
            return;
        }
    }
    if (g_end_skin_count >= END_REWARD_SKINS_MAX) return;
    g_end_skins[g_end_skin_count][0] = skin;
    g_end_skins[g_end_skin_count][1] = 1;
    g_end_skin_count++;
}

// io_challengeCompleted(index) at the end: a .endChalCard into #endChals ("Completed!")
void hud_ui_end_challenge_completed(const int icon, const char *name) {
    const HtmlElem holder = html_dom_by_id("endChals");
    if (!holder) return;
    char html[512];
    size_t len = 0;
    html[0] = 0;
    hb_append(html, sizeof(html), &len, "<div class='endChalCard' onclick='showWindow(33, true)' onmouseenter='playTick()'><img class='endChalImg' src='./img/chals/icon_%d.png' /><div class='endChalRight'>", icon);
    hb_append_escaped(html, sizeof(html), &len, name ? name : "");
    hb_append(html, sizeof(html), &len, "<div class='endChalDesc'>Completed!</div></div></div>");
    html_dom_insert_html(holder, HTML_INSERT_BEFORE_END, html);
}

// tabs.medals.gen(): medal_<i> on badge_0, the name and "x<count>"
static void hud_end_medals_html(char *out, const size_t size, size_t *len) {
    if (!g_end_medals.count) {
        hb_append(out, size, len, "<div style=\"text-align:center;color:#fff;font-size:30px;background-color:rgba(0, 0, 0, 0.4);border-radius:6px;padding:20px;\">You didnt earn any medals this game!</div>");
        return;
    }
    hb_append(out, size, len, "<div id='medalTable'>");
    for (int i = 0; i < g_end_medals.count; i++) {
        const int index = g_end_medals.medals[i].index;
        if (index >= PROGRESSION_MEDAL_COUNT) continue;
        hb_append(out, size, len, "<div class='medalCard'><div class='medalImg' style='background-image: url(./img/medals/medal_%d.png), url(./img/medals/badge_0.svg)'></div>"
                                  "<div class='medalTxt'>%s</div><div class='medalTxt'>x%d</div></div>", index, g_progression_medals[index], g_end_medals.medals[i].count);
    }
    hb_append(out, size, len, "</div>");
}

// tabs.rewards.gen(): matchRewardSummary (skins; KrunkNative has no junk)
static void hud_end_rewards_html(char *out, const size_t size, size_t *len) {
    if (!g_end_skin_count) {
        hb_append(out, size, len, "<div style=\"text-align:center;color:#fff;font-size:30px;background-color:rgba(0, 0, 0, 0.4);border-radius:6px;padding:20px;\">You didnt earn any rewards this game!</div>");
        return;
    }
    hb_append(out, size, len, "<div id='rewardTable'>");
    for (int i = 0; i < g_end_skin_count; i++) {
        const ViewerSkin *skin = viewer_skin(g_end_skins[i][0]);
        if (!skin) continue;
        char url[256];
        store_skin_preview(g_end_skins[i][0], url, sizeof(url));
        const int rarity = skin->rarity >= 0 && skin->rarity < g_viewer_rarity_count ? skin->rarity : 0;
        hb_append(out, size, len, "<div class='rewItemHolder' style='border: 2px solid %s;' onclick='viewItem(%d)'><img draggable='false' class='rewItemImg' loading='lazy' src='%s'><div class='rewItemN'>",
                  g_viewer_rarities[rarity].color, g_end_skins[i][0], url);
        hb_append_escaped(out, size, len, skin->name ? skin->name : "");
        hb_append(out, size, len, "</div><div class='rewItemCnt'>x%d</div></div>", g_end_skins[i][1]);
    }
    hb_append(out, size, len, "</div>");
}

// GUI.endScreen.show(): .endCard with the tab headers (not spectating) and the current tab.
static void hud_end_card_html(const Client *client, char *out, const size_t size) {
    size_t len = 0;
    out[0] = 0;
    // spectators (no player of ours: f1912's v11860 = true) get the leaderboard without headers
    const int spectator = spectate_active() || !client->me;
    hb_append(out, size, &len, "<div class=\"endCard\">");
    if (!spectator) {
        hb_append(out, size, &len, "<div id='tabHolder'>");
        for (int i = 0; i < END_TAB_COUNT; i++) {
            hb_append(out, size, &len, "<div class=\"tabHeader %s\"\n\t\t\t\t\t\tonmouseenter=\"playTick()\" onclick=\"GUI.endScreen.changeTab('%s')\">%s</div>",
                i == g_end_tab ? "selected" : "", END_TABS[i][0], END_TABS[i][1]);
        }
        hb_append(out, size, &len, "</div>");
    } else {
        g_end_tab = 0;
    }
    hb_append(out, size, &len, "\n\t\t\t\t\t<div class=\"tabBody\">");
    switch (g_end_tab) {
        case 1:
            hud_end_performance_html(client, out, size, &len);
            break;
        case 2: // tabs.medals: endData.mdls (NET_PACKET_END_MEDALS, WP-E part 2c)
            hud_end_medals_html(out, size, &len);
            break;
        case 3: // tabs.rewards: matchRewardSummary (challenge skins; none without an account)
            hud_end_rewards_html(out, size, &len);
            break;
        default:
            hud_end_leaderboard_html(client, out, size, &len);
            break;
    }
    hb_append(out, size, &len, "</div></div>");
}

static void hud_end_card_render(const Client *client) {
    char *html = malloc(32768);
    if (!html) return;
    hud_end_card_html(client, html, 32768);
    const HtmlElem view = html_dom_by_id("endTabbedView");
    if (view) html_dom_set_html(view, html);
    free(html);
}

static void hud_end_results(const Client *client);

// --render-scenario: the results at once (endscreen.results), without config.endAnim.
void hud_ui_end_results_now(const Client *client) {
    if (!g_hud_ui.ended || g_hud_ui.end_phase != 1) return;
    g_hud_ui.end_phase = 2;
    hud_end_results(client);
}

// The current tab again (the Battle Pass's bpData came in after the results were shown).
void hud_ui_end_refresh(void) {
    if (g_hud_ui.ended && g_hud_ui.end_phase == 2 && g_end_tab == 1 && g_end_client) hud_end_card_render(g_end_client);
}

// GUI.endScreen.changeTab(id) (the tab headers' onclick).
void hud_ui_end_screen_tab(const char *tab) {
    for (int i = 0; tab && i < END_TAB_COUNT; i++) {
        if (strcmp(tab, END_TABS[i][0]) != 0) continue;
        g_end_tab = i;
        if (g_hud_ui.ended && g_hud_ui.end_phase == 2 && g_end_client) hud_end_card_render(g_end_client);
        return;
    }
}

// f1912 + f1913 + f1915 + f1916: the results after config.endAnim.
static void hud_end_results(const Client *client) {
    dom_display("endInfo", "block");
    v10_rewards_end_screen(); // v10.0.2 endScreen.init: a guest's #matchEndSignupIncentives (v10_rewards.c)
    dom_style("endUI", "background-color", "transparent");
    dom_display("victorySub", "");              // f1913
    hud_end_title_hide();                       // overlay.vicTitle.show = false
    dom_style("endAHolderR", "opacity", "1");
    dom_style("endAHolderL", "opacity", "1");
    if (!account_get() || !account_get()->premium) { // the ad slots of a non-premium player
        dom_display("endAHolderL", "inline-block");
        dom_display("endAHolderR", "inline-block");
    } else {
        dom_display("endAHolderL", "none");
        dom_display("endAHolderR", "none");
    }
    // WP-E: f1915 endData.mts cards (lobby_events.c), then f1916's
    // `endMidHolder.style.top = mts.length <= 1 ? "205px" : "310px"`: the results start right below
    // the vote cards (#matchVoteHolder, top 220px + 100px). Without a top, #endMidHolder hangs from
    // its `bottom: 150px` and a short leaderboard leaves a wide gap under the votes.
    dom_style("endMidHolder", "top", lobby_vote_show() ? "310px" : "205px");
    // f1916: the team totals of a team mode
    const GameMode *mode = client->game.mode;
    if (mode && mode->config.teams) {
        char html[512];
        const char *n1 = mode->config.team_options[0] ? mode->config.team_options[0] : "Alpha";
        const char *n2 = mode->config.team_options[1] ? mode->config.team_options[1] : "Bravo";
        const int winner = g_end_result < 0 ? 0 : g_end_result ? (client->me ? client->me->team : 0) : (client->me ? 3 - client->me->team : 0);
        snprintf(html, sizeof(html), "%s<span class=\"teamTotalN0\">%s</span><span class=\"teamTotalScore\">%d</span>",
                 winner == 1 ? "<div class='material-icons teamWin0'>emoji_events</div>" : "", n1, mode->team_scores[1]);
        HtmlElem t = html_dom_by_id("teamTotal0");
        if (t) html_dom_set_html(t, html);
        snprintf(html, sizeof(html), "<span class=\"teamTotalScore\">%d</span><span class=\"teamTotalN1\">%s</span>%s",
                 mode->team_scores[2], n2, winner == 2 ? "<div class='material-icons teamWin1'>emoji_events</div>" : "");
        if ((t = html_dom_by_id("teamTotal1"))) html_dom_set_html(t, html);
        dom_display("teamTotal0", "inline-block");
        dom_display("teamTotal1", "inline-block");
    } else {
        dom_display("teamTotal0", "none");
        dom_display("teamTotal1", "none");
    }
    dom_display("endMidHolder", "inline-block");

    g_end_client = client;
    // GUI.endScreen.init(): changeTab(spectating || isCustom ? "leaderboard" : "performance")
    g_end_tab = spectate_active() || !client->me ? 0 : 1;
    hud_end_card_render(client);
    hud_v10_end_signup(1); // v10: a guest's signup incentive under the results (init's last callback)
}

// Per frame (overlay.render runs every frame): the results once config.endAnim ran out (f1912 ->
// f1913: vicTitle.show = false), else one vicTitle step. Its dt is the frame's gameTimeDiff:
// the frame time clamped to config.dltMx, then scaled by the round-end slow motion
// (endAnimationTimer / endAnim, menu_view_time_scale), as v7's overlay.render gets it.
// toggleMenu(true) hides #endUI, so an open menu takes the reset branch.
static void hud_end_step(const Client *client, const double now_ms) {
    HudUi *ui = &g_hud_ui;
    double dt = ui->tick_last_ms >= 0.0 ? now_ms - ui->tick_last_ms : 0.0;
    ui->tick_last_ms = now_ms;
    if (g_hud_clock_s >= 0.0 && dt > 1000.0 / 60.0 + 0.5) { // a scenario clock jump (replayed below)
        ui->tick_jump_ms = dt - 1000.0 / 60.0;
        dt = 1000.0 / 60.0;
    }
    dt = input_script_delta((float) (dt / 1000.0)) * 1000.0; // KRUNKNATIVE_FIXED_DELTA: the game's frame step
    dt = dt > 0.0 ? (dt < VIC_TITLE_DT_MAX_MS ? dt : VIC_TITLE_DT_MAX_MS) : 0.0;
    if (ui->ended && ui->end_phase == 1 && now_ms - ui->end_start_ms >= END_ANIM_MS) {
        ui->end_phase = 2;
        hud_end_results(client); // hides the title
    }
    const int show = ui->ended && ui->end_phase == 1 && !html_ui_is_menu_open();
    const double world = client && client->game.ready ? menu_view_time_scale() : 1.0;
    if (g_hud_clock_s >= 0.0 && show) {
        // --render-scenario: the pinned clock jumps to the capture time; replay the jump as the
        // 60 Hz frames it stands for, so a capture `after_ms` past the END shows that frame.
        double left = ui->tick_jump_ms;
        ui->tick_jump_ms = 0.0;
        for (; left > 0.0; left -= 1000.0 / 60.0) hud_end_title_frame(show, (left < 1000.0 / 60.0 ? left : 1000.0 / 60.0) * world);
    }
    hud_end_title_frame(show, dt * world);
}

void hud_ui_show_end_screen(Client *client, const int result, const char *next_round) {
    if (!html_ui_available() || !html_ui_is_ready()) return;
    HudUi *ui = &g_hud_ui;
    g_end_result = result;
    snprintf(g_end_timer, sizeof(g_end_timer), "%s", next_round ? next_round : "");
    if (!ui->ended) {
        // the END: showEndScreen + vicTitle.show; later calls only refresh the countdown.
        // hideUI(false) -> toggleMenu(false): a menu open at the END goes (UI.toggleWindow(false)
        // first); the pointer stays as it was, like v7's CONTROLS: free (and turning nothing)
        // through the title when the menu had it, until a click or the results (pointer_lock.h).
        if (html_ui_is_menu_open()) {
            html_ui_set_menu_open(0);
            menu_apply_open(0);
        }
        ui->ended = 1;
        ui->end_phase = 1;
        client_pointer_update(client);
        hud_v10_end_signup(0); // showEndScreen: matchEndSignupIncentives hidden until the results
        ui->end_start_ms = hud_now_ms(client);
        dom_style("endUI", "background-color", "");
        hud_end_screen_dom(1); // the title draws from this frame's HUD tick on (overlay.render after io_gameEnd)
        return;
    }
    char text[128];
    I18N_F(text, "timer.end", "Next Round in {0}", g_end_timer);
    dom_text("endTimer", text);
}

// 1 while the end screen shows its clickable results (Krunker frees the pointer at
// round end so the tabs can be clicked); main.c routes mouse input to the page then.
int hud_ui_end_screen_interactive(void) {
    return g_hud_ui.ended && g_hud_ui.end_phase == 2;
}

// 1 from the END to the next INIT (title animation or results): the end screen takes
// precedence over the menu. showEndScreen -> hideUI() sets UI.active = false, so losing the
// pointer lock (toggleControlUI(false)) shows no #instructionHolder; only toggleEndScreen
// (Escape once the results show) brings the menu in front.
int hud_ui_end_screen_up(void) {
    return g_hud_ui.ended && g_hud_ui.end_phase != 0;
}

// UI.toggleEndScreen() (UI.js), reached through pressButton(27) (Escape) and
// CONTROLS.toggle(true) while the controls are locked (controller Start). Only once the
// results show (hasEndScreen && #endInfo block): with the menu over them (#endUI hidden)
// they come back (#endUI/#endInfo/#victorySub shown, #menuHolder hidden, hideUI() - the
// visibility pass re-applies hud_end_screen_dom when the menu closes) and the pointer
// stays free for the results; otherwise toggleMenu(true). Returns 1 when it acted.
int hud_ui_toggle_end_screen(Client *client) {
    if (!client || !hud_ui_end_screen_up()) return 0;
    if (!hud_ui_end_screen_interactive()) return 1; // title animation: #endInfo not shown yet, nothing toggles
    if (html_ui_is_menu_open()) {
        client_set_menu_open(client, 0); // the results keep the pointer free: CONTROLS stay disabled (pointer_lock.h)
        dom_display("endUI", "");
        const HtmlElem base = html_dom_by_id("uiBase");
        if (base) html_dom_set_class(base, "onEndScrn", 1);
        dom_display("endInfo", "block");
        dom_display("victorySub", "");
        dom_display("menuHolder", "none");
    } else {
        client_set_menu_open(client, 1);
    }
    return 1;
}

// The next round started (net INIT): undo hud_ui_show_end_screen. Krunker drops the end
// screen through toggleMenu(true) (endUI display none) and, on spawn, toggleMenu(false) +
// toggleGameUI(true) + aV.hits.length = 0; the menu or the HUD comes back per the
// current state on the next tick.
void hud_ui_hide_end_screen(Client *client) {
    HudUi *ui = &g_hud_ui;
    if (!ui->ended) return;
    ui->ended = 0;
    ui->end_phase = 0;
    if (client) client_pointer_update(client); // the next round: locked again unless the menu is up
    g_end_timer[0] = 0;
    // WP-E part 2c: a new game: endData.mdls, matchRewardSummary = {} and endChals.innerHTML = ""
    memset(&g_end_medals, 0, sizeof(g_end_medals));
    g_end_skin_count = 0;
    battlepass_ui_end_clear(); // v7.1.4: the next end screen's bpData comes with its own commit
    if (!html_ui_available() || !html_ui_is_ready()) return;
    const HtmlElem chals = html_dom_by_id("endChals");
    if (chals) html_dom_set_html(chals, "");

    dom_display("endUI", "none");
    dom_display("endInfo", "none");
    dom_display("endMidHolder", "none");
    dom_display("victorySub", "none");
    hud_v10_end_signup(0);
    const HtmlElem base = html_dom_by_id("uiBase");
    if (base) html_dom_set_class(base, "onEndScrn", 0); // toggleMenu(true)
    hud_end_title_hide();
    hud_dmg_clear();
    if (html_ui_is_menu_open()) {
        menu_apply_open(1);      // toggleMenu(true): #menuHolder, instructions, chat at 20px
    } else if (!ui->dead) {
        hud_toggle_game_ui(1);   // toggleGameUI(true) (chat back at 148px)
    }
    ui->visible = -1;            // hud_apply_visibility rewrites #inGameUI & co next tick
    if (client) client->hud_html.valid = 0;
    ui->cross_shown = -1;
}

/* ------------------------------------------------------------ scenario override */

static int hud_json_int(const cJSON *obj, const char *key, const int fallback) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) return (int) lround(item->valuedouble);
    if (cJSON_IsBool(item)) return cJSON_IsTrue(item);
    return fallback;
}

static const Weapon *hud_weapon_by_name(const Game *game, const char *name) {
    if (!name || !name[0]) return NULL;
    const size_t count = game->weapon_count ? game->weapon_count : rules_weapon_count();
    const Weapon *const *weapons = game->weapons ? (const Weapon *const *) game->weapons : (const Weapon *const *) rules_weapons();
    for (size_t i = 0; i < count; i++) {
        if (weapons[i] && weapons[i]->name && !strcmp(weapons[i]->name, name)) return weapons[i];
    }
    return NULL;
}

// The `hud` payload of ui/js/hud.js (tools/uidiff/README.md) over the sampled values.
static void hud_apply_override(const Client *client, HtmlHudState *next, HudUiExtra *extra, int *ping, int *reloading) {
    const cJSON *h = g_hud_override.hud;
    const cJSON *item;
    int aim_pct_override = -1;
    if (cJSON_IsString(item = cJSON_GetObjectItemCaseSensitive(h, "weaponName"))) {
        const Weapon *weapon = hud_weapon_by_name(&client->game, item->valuestring);
        if (weapon) next->weapon = weapon;
    }
    next->health = hud_json_int(h, "health", next->health);
    next->max_health = hud_json_int(h, "maxHealth", next->max_health);
    next->health_segments = hud_json_int(h, "healthSegments", next->health_segments);
    next->ammo = hud_json_int(h, "ammo", next->ammo);
    next->ammo_max = hud_json_int(h, "ammoMax", next->ammo_max);
    next->ammo_limit = hud_json_int(h, "ammoLimit", next->ammo_limit);
    next->melee = hud_json_int(h, "melee", next->melee);
    next->reload_percent = hud_json_int(h, "reloadPercent", next->reload_percent);
    next->kills = hud_json_int(h, "kills", next->kills);
    next->deaths = hud_json_int(h, "deaths", next->deaths);
    next->score = hud_json_int(h, "score", next->score);
    next->class_index = hud_json_int(h, "classIndex", next->class_index);
    if (cJSON_IsBool(item = cJSON_GetObjectItemCaseSensitive(h, "scoped"))) {
        // hud.js `scoped` = aiming down sights. Only scope weapons show Krunker's reticle
        // (same rule as the sampled value below); the others aim through their own sight
        // and just hide the crosshair (aimVal 1).
        const Weapon *weapon = next->weapon;
        next->scoped = cJSON_IsTrue(item) && weapon && weapon->scope;
        if (cJSON_IsTrue(item) && !next->scoped) aim_pct_override = 100;
    }
    if (cJSON_IsString(item = cJSON_GetObjectItemCaseSensitive(h, "timerText")))
        snprintf(next->timer_text, sizeof(next->timer_text), "%s", item->valuestring);
    *reloading = hud_json_int(h, "reloading", *reloading || next->reload_percent > 0);
    *ping = hud_json_int(h, "ping", *ping);
    extra->spread = hud_json_int(h, "spread", extra->spread);
    if (cJSON_IsNumber(item = cJSON_GetObjectItemCaseSensitive(h, "aim"))) extra->aim_pct = (int) lround(CLAMP(item->valuedouble, 0.0, 1.0) * 100.0);
    if (aim_pct_override >= 0) extra->aim_pct = aim_pct_override;
    extra->scoreboard = hud_json_int(h, "scoreboard", extra->scoreboard);
    if (cJSON_IsArray(item = cJSON_GetObjectItemCaseSensitive(h, "loadout"))) {
        const cJSON *slot;
        extra->loadout_size = 0;
        cJSON_ArrayForEach(slot, item) {
            if (extra->loadout_size >= HUD_UI_LOADOUT_ALL) break;
            const cJSON *name = cJSON_GetObjectItemCaseSensitive(slot, "name");
            const cJSON *skin = cJSON_GetObjectItemCaseSensitive(slot, "skin"); // scenario: the worn store skin's index
            extra->skin[extra->loadout_size] = cJSON_IsNumber(skin) ? viewer_skin(skin->valueint) : NULL;
            extra->loadout[extra->loadout_size++] = hud_weapon_by_name(&client->game, cJSON_IsString(name) ? name->valuestring : NULL);
        }
        // Object Manager scenarios: the slots from loadoutBase on are extra weapons; without it the
        // rack keeps v7's HUD_UI_LOADOUT_MAX slots
        extra->loadout_base = hud_json_int(h, "loadoutBase", -1);
        if (extra->loadout_base < 0) {
            if (extra->loadout_size > HUD_UI_LOADOUT_MAX) extra->loadout_size = HUD_UI_LOADOUT_MAX;
            extra->loadout_base = extra->loadout_size;
        }
    }
    extra->loadout_index = hud_json_int(h, "loadoutIndex", extra->loadout_index);
    extra->can_throw = hud_json_int(h, "canThrow", extra->can_throw);
}

// The scenario's hud.timerText (io_updateTimer writes #specTimer with #timerVal's text), NULL none.
const char *hud_ui_override_timer(void) {
    const cJSON *t = g_hud_override.hud ? cJSON_GetObjectItemCaseSensitive(g_hud_override.hud, "timerText") : NULL;
    return cJSON_IsString(t) ? t->valuestring : NULL;
}

void hud_ui_set_override(const cJSON *hud) {
    cJSON_Delete(g_hud_override.hud);
    g_hud_override.hud = hud ? cJSON_Duplicate(hud, 1) : NULL;
}

void hud_ui_set_override_scoreboard(const cJSON *rows) {
    cJSON_Delete(g_hud_override.scoreboard);
    g_hud_override.scoreboard = rows ? cJSON_Duplicate(rows, 1) : NULL;
}

void hud_ui_set_clock(const double now_s) {
    g_hud_clock_s = now_s;
}

void hud_ui_hit_elapsed(Client *client, const int kill, const double elapsed_ms) {
    hud_ui_hit(client, kill);
    if (!g_hud_ui.hit_active) return;
    g_hud_ui.hit_start_ms -= elapsed_ms;
    hud_hit_step(g_hud_ui.hit_start_ms + elapsed_ms);
}

/* ------------------------------------------------------------ visibility */

// A boolean setting as the settings store has it (saved value, else the page default).
static int hud_setting_on(const char *key) {
    const cJSON *v = ui_setting_value(key);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    if (cJSON_IsNumber(v)) return v->valuedouble != 0.0;
    return cJSON_IsString(v) && v->valuestring && !strcmp(v->valuestring, "true");
}

// Minimap.toggle(game, on): the top-left layout of the minimap's state (hud_map.c).
static void hud_minimap_layout(void) {
    hud_minimap_apply_layout();
    dom_display("timerHolder", "block");
}

// Mirrors UI.toggleGameUI(true) + toggleControlUI(true) with v7's default settings (DOM of
// its sandbox game, FFA/TDM, guest).
static void hud_apply_visibility(Client *client, const int in_game, const int visible) {
    HudUi *ui = &g_hud_ui;
    HtmlElem el;

    // #uiBase.onGame (main.css: the chat's in-game place) is UI.toggleControlUI(controls.enabled):
    // set while the game has the pointer, dropped when the menu opens over it (ESC pause, where
    // .onMenu puts the chat back beside the menu). The death screen keeps it (v7: "onGame
    // onDeathScrn"; .onDeathScrn's later rule places the chat). Mobile controls also toggle
    // .onGameM (the chat is hidden under the touch controls).
    (void) in_game;
    const int on_game = visible;
    if (on_game != ui->in_game) {
        ui->in_game = on_game;
        if ((el = html_dom_by_id("uiBase"))) {
            html_dom_set_class(el, "onGame", on_game);
            if (ui_mobile_active()) html_dom_set_class(el, "onGameM", on_game);
            else html_dom_set_class(el, "onGameM", 0);
        }
    }
    // #chatUI: v7 only ever writes it from the showUI setting (settings.js showUI.set:
    // chatUI.style.display = val ? "block" : "none"), so the chat is up beside the menu (also
    // before the first spawn), the death screen and the end screen.
    const int chat = client_render_settings()->show_ui;
    if (chat != ui->chat_shown) {
        ui->chat_shown = chat;
        dom_display("chatUI", chat ? "block" : "none");
    }
    // showUI off (UI.hideGameUI): toggleControlUI keeps #inGameUI, #customGUIHolder, #overlay
    // and #vignette hidden; #aimRecticle still follows the game. A live change re-applies.
    const int show_ui = client_render_settings()->show_ui;
    if (show_ui != ui->show_ui) {
        ui->show_ui = show_ui;
        ui->visible = -1;
    }
    // A map / mode change with no hidden frame in between (the offline sandbox reloads and
    // respawns inside one frame, net/offline.c): #matchInfo follows it.
    static int info_key = -1;
    const int key = client->game.ready ? client->game.current_map_index * 1024 + game_mode_id(&client->game) + 1 : -1;
    if (key != info_key) {
        info_key = key;
        if (visible && visible == ui->visible) hud_render_game_info(client);
    }
    if (visible == ui->visible) return;
    ui->visible = visible;
    g_hud_layout_epoch++;

    html_dom_set_class(html_dom_root(), "kn-hud-visible", visible && show_ui); // its !important rule would override showUI

    const char *block = visible ? "block" : "none";
    const char *game_block = visible && show_ui && !ui->hide_default ? "block" : "none";
    // toggleControlUI(visible)
    dom_display("inGameUI", game_block);
    dom_display("customGUIHolder", game_block); // KrunkScript's GUI (its children are the script's)
    dom_display("aimRecticle", block);
    dom_display("overlay", game_block);
    dom_display("vignette", game_block);
    dom_display("centerLeaderDisplay", "none");
    ui->board_shown = 0;
    dom_display("speakerDisplay", visible ? "block" : "none");
    if (visible) dom_display("infoHolder", "none");
    // toggleGameUI(true)
    dom_display("gameMessage", block);
    dom_display("roundMessage", block);
    dom_display("topLeftMain", visible ? "inline-block" : "none");
    dom_display("topLeftMap", block);
    dom_display("topLeftMatchData", block);
    dom_display("topLeftData", visible ? "inline-block" : "none");
    dom_display("topLeftBottom", block);
    dom_display("topRight", block);
    dom_display("bottomLeftHolder", block);
    dom_display("botRightHider", block);
    dom_display("weapHolder", visible && !ui_mobile_active() ? "grid" : "none");
    dom_display("bottomRight", block);
    dom_display("deathUIHolder", "none");
    dom_style("speakerDisplay", "right", visible ? "380px" : "20px");
    hud_minimap_layout();

    // Pieces the Display settings own (settings.js `set`s; ui_render_settings.c applies live
    // changes) are written from the same values, so this pass never undoes a setting.
    const ClientRenderSettings *rs = client_render_settings();
    dom_display("leaderboardHolder", "block"); // v10: always shown (switchLeaderboard, hud_v10.c)
    dom_display("killCount", rs->show_kill_c ? "inline-block" : "none");       // showKillC
    dom_display("deathCount", rs->show_deaths ? "inline-block" : "none");      // showDeaths
    dom_display("streakCount", rs->show_streak ? "inline-block" : "none");     // showStreak
    dom_display("kdCount", hud_setting_on("showKD") ? "inline-block" : "none"); // showKD
    dom_display("scoreCount", rs->show_score ? "inline-block" : "none");       // showScore
    dom_display("fpsDisplay", menu_ui()->settings.show_fps ? "block" : "none"); // showFPS
    dom_display("scoreZoneCount", "none");
    dom_display("livesCount", "none");
    dom_display("ingressDisplay", rs->show_net_stat ? "block" : "none"); // showNetStat
    dom_display("egressDisplay", rs->show_net_stat ? "block" : "none");
    dom_display("tickDisplay", rs->show_net_stat ? "block" : "none");
    dom_display("propControlHolder", "none");
    dom_display("interactMsg", "none");
    dom_display("teamScores", "none");
    dom_display("bhHolder", "none");
    dom_display("bloodDisplay", "none");
    dom_display("pingDisplay", rs->show_ping ? "block" : "none");      // showPing
    dom_display("killFeed", !rs->old_scoreboard && rs->show_kills ? "inline-grid" : "none"); // switchLeaderboard
    dom_display("ammoHolder", "inline-block");
    if ((el = html_dom_by_id("bottomLeftChallIcon"))) { // init: challIconSkull with challenges, else challIconHP
        html_dom_set_class(el, "challIconSkull", 0);
        html_dom_set_class(el, "challIconHP", 1);
    }
    if ((el = html_dom_by_id("chatList"))) {
        char value[32];
        fmt_opacity(rs->chat_opacity, value, sizeof(value));
        html_dom_set_style(el, "opacity", value);                        // chatOp
        snprintf(value, sizeof(value), "%gpx", rs->chat_height * 100.0);
        html_dom_set_style(el, "max-height", value);                     // chatHeight
        ui_controls_apply_chat_visibility();                             // shwChat: all / on focus / off
    }
    dom_text("reloadMsg", I18N("game.reload.keyboard", "[R] Reload")); // startBootstrapLoading

    if (visible) {
        hud_ensure_crosshair();
        hud_ensure_hitmarker();
        hud_render_game_info(client);
        client->hud_html.valid = 0;           // re-render every value group this frame
        client->hud_html.next_emit_time = 0.0;
    } else {
        dom_style("aimRecticle", "opacity", "0");
        hud_hide_hit();
        hud_dmg_hide_all();
    }
    if (visible && ui->dead) {
        hud_toggle_game_ui(0);
        dom_display("deathUIHolder", "block");
    }
    // The end screen stays down while the menu is over it (UI.toggleEndScreen ->
    // toggleMenu(true) hid #endUI); it comes back when the menu closes.
    if (ui->ended && !html_ui_is_menu_open()) hud_end_screen_dom(0);
}

// KrunkScript game.ui hideDefault() (v7 createKrunkScriptApiContext.js): game.ui.hidden, and
// #gameUIHider / #fullMenHider hidden; `hidden` 0 shows them again.
void hud_ui_hide_default(const int hidden) {
    g_hud_ui.hide_default = !!hidden;
    dom_display("gameUIHider", hidden ? "none" : "");
    dom_display("fullMenHider", hidden ? "none" : "block");
}

// UI.toggleWholeUI(on): the forceHidden class on #fullMenHider, #customGUIHolder, #gameUIHider.
void hud_ui_toggle_whole_ui(const int on) {
    static const char *const ids[] = {"fullMenHider", "customGUIHolder", "gameUIHider"};
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        const HtmlElem el = html_dom_by_id(ids[i]);
        if (el) html_dom_set_class(el, "forceHidden", !on);
    }
}

/* ------------------------------------------------------------ autoplay (test hook) */

// `?<name>=1` on the web, KRUNKNATIVE_<NAME>=1 natively.
static int hud_test_flag(const char *query_name, const char *env_name) {
#ifdef __EMSCRIPTEN__
    (void) env_name;
    return EM_ASM_INT({
        var value = null;
        try { value = new URLSearchParams(location.search).get(UTF8ToString($0)); } catch (e) {}
        return value !== null && value !== '0' && value !== 'false' ? 1 : 0;
    }, query_name);
#else
    (void) query_name;
    const char *value = getenv(env_name);
    return value && value[0] && strcmp(value, "0") != 0 && strcmp(value, "false") != 0;
#endif
}

// kn_hud_demo: exercise the game-event entry points without game events (kill feed
// rows every 3 s so a screenshot catches them alive, a hit marker each time).
static void hud_demo(Client *client, const double now) {
    HudUi *ui = &g_hud_ui;
    if (!ui->demo || ui->visible != 1 || now < ui->demo_next) return;

    ui->demo_next = now + 3.0;
    hud_ui_add_killfeed(client, client->menu_name, "Guest_4", "icon_2", 1);
    hud_ui_add_killfeed(client, "Guest_7", "Guest_4", "icon_1", 0);
    hud_ui_add_killfeed(client, "Guest_4", client->menu_name, NULL, 0);
    hud_ui_add_killfeed_suicide(client, "Guest_9", 0);
    hud_ui_hit(client, ((int) now) % 2);
    // Damage from 10 units straight ahead and 10 units to the right of the view.
    const Player *me = client->me;
    const float yaw = me->direction.y;
    hud_ui_damage_from(client, me->position.x - 10.0f * sinf(yaw), me->position.z - 10.0f * cosf(yaw));
    hud_ui_damage_from(client, me->position.x + 10.0f * cosf(yaw), me->position.z - 10.0f * sinf(yaw));
    // A damage number 20 units ahead at eye height (NJ2 adds 7 to y).
    hud_ui_damage_number(client, me->position.x - 20.0f * sinf(yaw), me->position.y, me->position.z - 20.0f * cosf(yaw), 37, ((int) now) % 2);
}

// TEMPORARY (test only, autoplay path): until menu_ui.c's menu_apply_open() hides the
// menu in engine builds, nothing takes #menuHolder off the screen after
// client_set_menu_open(0). Mirrors the hide half of krunknative.js applyMenuOpen(false)
// and skips itself as soon as the menu logic has done its job.
static void hud_autoplay_hide_menu(void) {
    const HtmlElem holder = html_dom_by_id("menuHolder");
    char display[16];
    if (!holder || html_dom_get_style(holder, "display", display, sizeof(display)) < 0 || strcmp(display, "block") != 0) return;

    printf("[hud_ui] autoplay: hiding the menu markup (menu_apply_open not active)\n");
    html_dom_set_class(html_dom_root(), "kn-menu-open", 0);
    html_dom_set_display(holder, "none");
    dom_display("menuHider", "none");
    dom_display("instructionHolder", "none");
    dom_display("gameNameHolder", "none");
    dom_display("spectButton", "none");
    dom_display("speakerDisplay", "block");
    const HtmlElem base = html_dom_by_id("uiBase");
    if (base) html_dom_set_class(base, "onMenu", 0);
}

static void hud_autoplay(Client *client) {
    HudUi *ui = &g_hud_ui;
    if (!ui->autoplay_checked) {
        ui->autoplay_checked = 1;
        ui->autoplay = hud_test_flag("kn_autoplay", "KRUNKNATIVE_AUTOPLAY");
        ui->demo = hud_test_flag("kn_hud_demo", "KRUNKNATIVE_HUD_DEMO");
    }
    if (!ui->autoplay || ui->autoplay_done || client->in_game || !client->game.ready || !hud_connected(client)) return;

    ui->autoplay_done = 1;
    // KRUNKNATIVE_AUTOPLAY_SPECTATE=1 / ?kn_autoplay_spectate=1: enter as a spectator (toggleSpect)
    if (hud_test_flag("kn_autoplay_spectate", "KRUNKNATIVE_AUTOPLAY_SPECTATE")) spectate_set_requested(1);
    printf("[hud_ui] autoplay: entering the game\n");
    client_set_menu_open(client, 0);
    client_enter_game(client);
    hud_autoplay_hide_menu();
    // Headless browsers refuse the pointer lock, which would make client_menu_tick()
    // reopen the menu after its grace period and hide the HUD again: keep the game
    // owning the input for this (test-only) session.
    client->menu_lock_grace_until = 1e18;
}

/* ------------------------------------------------------------ tick */

// io_effect / io_killStreak / io_updateWeaponStreak (v7 index.js 15319-15395) from the local
// player's mirrored state (net/combat_events.c): #perkHolder (one .perkItem per effect with
// stacks, img/perks/<effect>.png; v7's markup has the `<di class="perkItemInner">` typo, kept so
// the parser builds the same tree), #killStreakHolder (owned streaks by loadout slot, then kills:
// key name, x<count>, img/streaks/<index>.png; mobile: ontouchstart callInStreak, no key) and the
// rack's .weapStreakHolder of the streak weapon. Rewritten only when the state changes.

static void hud_combat_reset(void) {
    g_combat_hash = 0;
    dom_text("perkHolder", "");
}

static void hud_combat_step(const Client *client) {
    const Player *me = client->me;
    if (!me) return;
    const int mobile = ui_mobile_active();
    unsigned h = 2166136261u;
#define HASH_INT(v) (h = (h ^ (unsigned) (v)) * 16777619u)
    for (int i = 0; i < EFFECT_COUNT; i++) HASH_INT(me->effects[i]);
    for (int i = 0; i < STREAK_COUNT; i++) HASH_INT(me->streaks_owned[i]);
    HASH_INT(me->streak_weapon);
    HASH_INT(me->weapon_streak);
    HASH_INT(mobile);
    for (int i = 0; i < STREAK_SLOTS; i++) HASH_INT(ui_controls_bind(UI_CONTROL_STREAK0 + i));
    HASH_INT(g_hud_ui.extra.loadout_size); // a rebuilt rack lost its streak counters
    HASH_INT(g_hud_layout_epoch);
#undef HASH_INT
    if (!h) h = 1;
    if (h == g_combat_hash) return;
    g_combat_hash = h;

    char html[2048];
    size_t len = 0;
    html[0] = 0;
    for (int i = 0; i < EFFECT_COUNT; i++) {
        if (!me->effects[i]) continue;
        hb_append(html, sizeof(html), &len, "<div class='perkItem'><di class=\"perkItemInner\"><img src=\"img/perks/%d.png\" style=\"width: 100%%;\">"
                                            "<div class='perkCnt'>x%d</div></div></div>", i, me->effects[i]);
    }
    const HtmlElem perks = html_dom_by_id("perkHolder");
    if (perks) html_dom_set_html(perks, html);

    // [...Game.streaks].sort(by kills).sort(by slot): stable, so slot order with kills inside
    int order[STREAK_COUNT];
    for (int i = 0; i < STREAK_COUNT; i++) {
        int at = i;
        while (at > 0 && (g_streaks[order[at - 1]].slot > g_streaks[i].slot ||
                          (g_streaks[order[at - 1]].slot == g_streaks[i].slot && g_streaks[order[at - 1]].kills > g_streaks[i].kills))) {
            order[at] = order[at - 1];
            at--;
        }
        order[at] = i;
    }
    len = 0;
    html[0] = 0;
    for (int k = 0; k < STREAK_COUNT; k++) {
        const int i = order[k], slot = g_streaks[i].slot;
        if (!me->streaks_owned[i]) continue;
        hb_append(html, sizeof(html), &len, "<div class='killStreakItem'");
        if (mobile) hb_append(html, sizeof(html), &len, " ontouchstart='callInStreak(%d)'", slot);
        hb_append(html, sizeof(html), &len, "><div class='killStreakItemInner'>");
        if (!mobile) {
            char key[32];
            hb_append(html, sizeof(html), &len, "<div class='killStreakKey'>");
            hb_append_escaped(html, sizeof(html), &len, ui_controls_key_name(ui_controls_bind(UI_CONTROL_STREAK0 + slot), key, sizeof(key)));
            hb_append(html, sizeof(html), &len, "</div>");
        }
        hb_append(html, sizeof(html), &len, "<div class='killStreakCnt'>x%d</div><img style='width: 100%%;' src=\"img/streaks/%d.png\"></div></div>",
                  me->streaks_owned[i], i);
    }
    const HtmlElem tray = html_dom_by_id("killStreakHolder");
    if (tray) html_dom_set_html(tray, html);

    const HtmlElem rack = html_dom_by_id("weapHolder");
    for (HtmlElem item = rack ? html_dom_first_child(rack) : 0; item; item = html_dom_next(item)) {
        const HtmlElem holder = html_dom_query(item, ".weapStreakHolder");
        if (!holder) continue;
        char idx[16] = "";
        html_dom_get_attr(holder, "data-weapon-index", idx, sizeof(idx));
        const int on = idx[0] && atoi(idx) == me->streak_weapon;
        html_dom_set_display(holder, on ? "block" : "none");
        const HtmlElem count = html_dom_query(holder, ".weapStreak");
        char text[16];
        snprintf(text, sizeof(text), "%d", on ? me->weapon_streak : 0);
        if (count) html_dom_set_text(count, text);
    }
}

// f1778 (window.updatePacketLoop, every 1000 ms while showNetStat): the io-client tallies
// since the last call, then resetPacketTally. Runs on the menu too, like v7's interval.
static void hud_net_stats_step(const double now_s) {
    static double next_s = -1.0;
    static int shown = -1;
    const int on = client_render_settings()->show_net_stat;
    if (!on) {
        shown = 0;
        return;
    }
    if (shown != 1 || next_s < 0.0 || now_s < next_s - 1.0) next_s = now_s; // just enabled (or the clock jumped back)
    shown = 1;
    if (now_s < next_s) return;
    next_s = now_s + 1.0;
    ClientNetTally t;
    client_net_tally_take(&t);
    char text[48];
    snprintf(text, sizeof(text), "%d P/s", t.ingress_packets);
    dom_text("ingressPacketCount", text);
    snprintf(text, sizeof(text), "%.2f KB/s", t.ingress_bytes / 1000.0);
    dom_text("ingressDataSize", text);
    snprintf(text, sizeof(text), "%d P/s", t.egress_packets);
    dom_text("egressPacketCount", text);
    snprintf(text, sizeof(text), "%.2f KB/s", t.egress_bytes / 1000.0);
    dom_text("egressDataSize", text);
    snprintf(text, sizeof(text), "%d TPS", t.ticks);
    dom_text("tickPacketCount", text);
}

// While spectating (window.Bf5, no live player of ours): hud_spectate.c shows #spectateUI
// and keeps the player holders hidden; this keeps the shared pieces live at 20 Hz:
// the sBoardKey leaderboard (#newLeaderDisplay), the round clock and ping. Kill feed and
// chat rows arrive through their own entry points. Player-only overlays stay off.
// f1987 outside the playing HUD (spectating, the death screen): v7 polls the board key before
// its tmpPlayer.active test, so the board follows Tab there too (the player tick renders it
// from its value groups while alive).
static void hud_board_step(const Client *client) {
    HudUi *ui = &g_hud_ui;
    const int board = !ui->ended && hud_scoreboard_key(client);
    char board_html[4096];
    board_html[0] = 0;
    if (board) hud_scoreboard_html(client, g_hud_override.scoreboard, board_html, sizeof(board_html));
    if (board != ui->board_shown || (board && hud_hash(board_html) != ui->board_hash)) hud_render_scoreboard(board, board_html);
}

static void hud_spectator_tick(Client *client, const double now) {
    HudUi *ui = &g_hud_ui;
    HtmlHudState *hud = &client->hud_html;

    if (ui->dmg_last_ms >= 0.0) hud_dmg_hide_all();
    if (ui->nums_last_ms >= 0.0) hud_nums_hide_all();
    hud_speed_step(client, now * 1000.0); // ui->visible != 1: hides it
    if (ui->hit_active) hud_hide_hit();
    // The overlay draws the crosshair only `!window.spectating` (hud_spectate.c hides
    // #knCrosshair too): keep cross_shown in step so playing again shows it.
    hud_hide_crosshair();
    if (now < hud->next_emit_time) return;
    hud->next_emit_time = now + HUD_UI_MIN_INTERVAL;

    hud_board_step(client);

    char timer[sizeof(hud->timer_text)];
    hud_timer(client, timer, sizeof(timer));
    if (strcmp(timer, hud->timer_text) != 0) {
        snprintf(hud->timer_text, sizeof(hud->timer_text), "%s", timer);
        hud_render_timer(hud->timer_text);
    }
    const int ping = client->net_ping_ms >= 0 ? client->net_ping_ms : -1;
    if (ping != ui->ping) {
        hud_render_ping(ping);
        ui->ping = ping;
    }
    hud->valid = 0; // the player HUD rewrites everything when we play again
}

void hud_ui_relayout(void) {
    g_hud_ui.visible = -1;
    // .onGame too: spectating writes it behind this cache (hud_spectate.c), and when spectating
    // ends with no menu toggle (disconnect, INIT) the next pass must drop it (toggleControlUI)
    g_hud_ui.in_game = -1;
}

// The page parts of a round (INIT, and a fresh page during one).
static void hud_round_page(Client *client) {
    comp_ui_round(client); // f1904(Game.isComp): the comp lobby's menu or the default one
    if (!client->game.config.comp) ranked_ui_left_match(); // a round outside a comp lobby: no ranked match any more
    // io_startGame: `if (chatChannel != 1 && Game.isComp) switchChat(chatSwitch)`: a comp lobby
    // (ranked) starts on the team channel
    if (client->game.config.comp && g_chat_channel != CHAT_CHANNEL_TEAM && client->game.mode && client->game.mode->config.teams) hud_switch_chat();
}

void hud_ui_on_init(Client *client) {
    hud_ui_relayout();
    hud_ui_on_config(client);
}

void hud_ui_on_config(Client *client) {
    // before the page's first ready tick hud_reset_page does it
    if (client && html_ui_available() && html_ui_is_ready() && client->hud_html.was_ready) hud_round_page(client);
}

// A fresh page (ready edge): nothing we created or wrote still exists.
static void hud_reset_page(Client *client) {
    HudUi *ui = &g_hud_ui;
    hud_round_page(client);

    hud_feed_clear(0);
    hud_popups_page_reset();
    ui->visible = -1;
    ui->in_game = -1;
    ui->crosshair = 0;
    ui->hitmarker = 0;
    ui->hit_active = 0;
    ui->cross_shown = -1;
    ui->cross_size[0] = 0;
    ui->cross_opacity[0] = 0;
    ui->cross_def_opacity[0] = 0;
    ui->cross_sig[0] = 0;
    ui->cross_def = 0;
    ui->cross_cus = 0;
    ui->cross_dirty = 1;
    ui->chat_seeded = 0;
    ui->chat_shown = -1;
    ui->ping = INT_MIN;
    ui->dead = 0;
    ui->ended = 0;
    ui->board_shown = -1;
    ui->old_board = -1;
    ui->old_board_hash = 0;
    for (int i = 0; i < DMG_MAX; i++) ui->dmg[i] = (DamageHit) {0};
    ui->dmg_last_ms = -1.0;
    for (int i = 0; i < DMG_NUM_MAX; i++) ui->nums[i] = (DamageNum) {0};
    ui->nums_last_ms = -1.0;
    ui->speed = 0;
    ui->speed_shown = -1;
    ui->show_ui = -1;
    ui->end_phase = 0;
    ui->end_title = 0;
    ui->end_title_key[0] = 0;
    hud_end_title(); // the title images load with the page (game-overlay.js preloads them)
    client->hud_html.valid = 0;

    const HtmlElem base = html_dom_by_id("uiBase");
    if (base) html_dom_observe_resize(base, on_ui_base_resize, NULL);
}

// Per frame from client_menu_tick(): sample fps, keep the HUD visibility in step with
// the game/menu state, step the hit marker and, at most 20 Hz, rewrite the value
// groups that changed since the last write.
void client_hud_html_tick(Client *client, const float now_f) {
    if (!html_ui_available()) return;
    hud_v10_tick(); // every frame, also on the menu / while spectating (#chatUIHolder)

    HudUi *ui = &g_hud_ui;
    HtmlHudState *hud = &client->hud_html;
    const double now = g_hud_clock_s >= 0.0 ? g_hud_clock_s : now_f;
    const int ready = html_ui_is_ready();
    int fps_changed = 0;
    const int fps_before = hud->fps;

    hud_sample_fps(hud, now, &fps_changed);
    const cJSON *ov = g_hud_override.hud;
    if (ov) {
        // A scenario `fps` pins the value; without one the sampled fps keeps updating.
        hud->fps = hud_json_int(ov, "fps", hud->fps);
        fps_changed = hud->fps != fps_before;
    }

    if (ready && !hud->was_ready) hud_reset_page(client);
    if (!client->game.ready) g_leader_stats_valid = 0; // a map load / disconnect: the next server resends the board
    hud->was_ready = ready;
    if (!ready) {
        hud->valid = 0;
        return;
    }

    hud_autoplay(client);
    hud_net_stats_step(now);
    hud_end_step(client, now * 1000.0);
    hud_feed_step(now * 1000.0);
    // game-overlay text popups (client/src/ui/hud_popups.c): drawn while playing (no kill card / menu)
    hud_popups_set_comp(client->game.config.comp);
    hud_popups_tick(now * 1000.0, hud_in_game(client) && !html_ui_is_menu_open());

    // hud.js state.inGame came from the `state` event (client->in_game); the values
    // below are only sampled while the local player is alive.
    const int in_game = client->in_game && client->me;
    if (in_game && !ui->in_game_was) { // new session: welcome lines again, fresh head-to-head counts
        ui->chat_seeded = 0;
        g_h2h_count = 0;
    }
    ui->in_game_was = in_game;
    // the life the death screen's stats describe starts at the spawn
    static int alive_was;
    const int alive = hud_in_game(client);
    if (alive && !alive_was) {
        g_life_start_ms = now * 1000.0;
        g_life_kills = 0;
        g_life_score_start = client->me->score;
    }
    alive_was = alive;
    hud_apply_visibility(client, in_game, in_game && !html_ui_is_menu_open());
    hud_v7modes_tick(client, in_game, g_hud_layout_epoch); // rounds, KR tags, big head, speed run (hud_v7modes.c)
    social_prop_hud_tick(client, in_game && g_hud_ui.visible > 0, g_hud_layout_epoch); // WP-E: prop controls + taunt keys (hud_social.c)
    hud_gui_update(client); // map-G: the map's custom "gui" elements in #customGUIHolder (hud_gui.c)
    ui_notify_tick(now * 1000.0); // v3.1.1 mod notify.ts toast (ui_notify.c)

    if (spectate_active() && !hud_in_game(client)) {
        hud_spectator_tick(client, now);
        return;
    }
    if (ui->visible != 1 || !hud_in_game(client)) {
        // Dead (kill() -> toggleAim(player, 0), the overlay canvas hidden with the death
        // card), menu or no player: no crosshair, scope or sight reticle. A respawn or
        // closing the menu rewrites them (valid = 0 below).
        if (hud->scoped || hud->aim_dot) hud_reset_aim(client);
        hud_hide_crosshair();
        // the hit marker too: its fade (hud_hit_step) only runs while playing, so a marker from
        // the last hits before the death stayed on the death screen, frozen at its opacity
        if (ui->hit_active) hud_hide_hit();
        if (ui->dmg_last_ms >= 0.0) hud_dmg_hide_all();
        if (ui->nums_last_ms >= 0.0) hud_nums_hide_all();
        hud_speed_step(client, now * 1000.0); // hides it
        hud->valid = 0; // TODO: death screen / end screen (#killCardHolder, #endUI) once the game exposes them
        if (ui->visible == 1 && ui->dead && now >= hud->next_emit_time) { // the death screen: Tab still shows the board
            hud->next_emit_time = now + HUD_UI_MIN_INTERVAL;
            hud_board_step(client);
        }
        return;
    }

    hud_demo(client, now);
    if (!ov) hud_combat_step(client); // perks, streak tray, weapon streak (scenarios leave them empty)
    hud_hit_step(now * 1000.0);
    hud_dmg_step(client, now * 1000.0);
    hud_nums_step(client, now * 1000.0);
    hud_speed_step(client, now * 1000.0);

    if (now < hud->next_emit_time) return;
    hud_old_board_step(client);

    const Player *me = client->me;
    const Game *game = &client->game;
    const Weapon *weapon = me->weapon;
    const int force = !hud->valid;

    HtmlHudState next = *hud;
    HudUiExtra next_extra = ui->extra;
    next.health = (int) lroundf(me->health);
    next.max_health = me->max_health;
    next.health_segments = hud_health_segments(client, me);
    next.ammo = hud_ammo(me);
    next.ammo_max = weapon ? (int) weapon->ammo : 0;
    next.ammo_limit = game->mode && game->mode->config.ammo_limit > 0;
    next.melee = weapon ? weapon->melee : 0;
    next.reload_percent = hud_reload_percent(me, game);
    next.kills = (int) me->kills;
    next.deaths = (int) me->deaths;
    next.score = me->score;
    next.class_index = me->class_index;
    // 95.js hvJK5: #aimRecticle only for `scope` weapons, once fully aimed (jKFi4 == 0);
    // vfx.c hides the muzzle sprite / overlay flash / casings on the same condition.
    // Both are drawn on the overlay canvas by default, which the end screen hides (hideUI).
    next.scoped = weapon && weapon->scope && me->aim_val >= 1.0f && !ui->ended;
    // 95.js toggleAim: with an attachment (and the normal first-person camera) the reticle image
    // (textures/reticles/reticle_<reticleIndex>.png, drawn centred by 91.js) shows once fully aimed.
    // aiming cancels an inspect in v3.1.1, so the dot never shows during one; keep that explicit.
    // KrunkNative's third person aims behind the shoulder: v10 toggleAim shows the dot there too
    // (v10.js 172054, only camType.isNormal); the host's own thirdPerson keeps v7's rule.
    next.aim_dot = weapon && weapon_attachment(weapon, DEFAULT_ATTACHMENT_INDEX) && me->aim_val >= 1.0f && !game->config.third_person && !me->inspecting && !ui->ended;
    next.weapon = weapon;
    hud_timer(client, next.timer_text, sizeof(next.timer_text));
    next_extra.spread = (int) lroundf(hud_spread(me));
    next_extra.aim_pct = (int) lroundf(CLAMP(me->aim_val, 0.0f, 1.0f) * 100.0f);
    // v10 updateCrosshair (v10.js 221608): with a camera offset (thirdPerson) the crosshair stays
    // while aiming, except with a scope (its overlay takes over); so does KrunkNative's third person
    if (third_person_view(client) && !(weapon && weapon->scope)) next_extra.aim_pct = 0;
    next_extra.scoreboard = hud_scoreboard_key(client) && !client_render_settings()->old_scoreboard; // g6: leaderType == 1 only
    hud_loadout(me, &next_extra);
    int ping = client->net_ping_ms >= 0 ? client->net_ping_ms : -1; // Krunker shows "-" for -1
    int reloading = next.reload_percent > 0;
    if (ov) hud_apply_override(client, &next, &next_extra, &ping, &reloading);

    const int health_changed = force || next.health != hud->health || next.max_health != hud->max_health || next.health_segments != hud->health_segments;
    const int ammo_changed = force || next.ammo != hud->ammo || next.ammo_max != hud->ammo_max || next.ammo_limit != hud->ammo_limit ||
                             next.melee != hud->melee || next.reload_percent != hud->reload_percent;
    const int timer_changed = force || strcmp(next.timer_text, hud->timer_text) != 0;
    const int counters_changed = force || next.kills != hud->kills || next.deaths != hud->deaths || next.score != hud->score;
    const int class_changed = force || next.class_index != hud->class_index;
    const int scope_changed = force || next.scoped != hud->scoped;
    const int aim_dot_changed = force || next.aim_dot != hud->aim_dot;
    const int loadout_rebuild = force || next_extra.loadout_size != ui->extra.loadout_size || next_extra.loadout_base != ui->extra.loadout_base ||
                                memcmp(next_extra.loadout, ui->extra.loadout, sizeof(next_extra.loadout)) != 0 ||
                                memcmp(next_extra.skin, ui->extra.skin, sizeof(next_extra.skin)) != 0 || // skins: the label (weapHolder lastSkins)
                                next_extra.melee_icon != ui->extra.melee_icon;
    const int loadout_changed = loadout_rebuild || next_extra.loadout_index != ui->extra.loadout_index || next.weapon != hud->weapon ||
                                next_extra.can_throw != ui->extra.can_throw;
    char board_html[4096];
    board_html[0] = 0;
    if (next_extra.scoreboard) hud_scoreboard_html(client, g_hud_override.scoreboard, board_html, sizeof(board_html));
    const int scoreboard_changed = force || next_extra.scoreboard != ui->extra.scoreboard ||
                                   (next_extra.scoreboard && (ui->board_shown != 1 || hud_hash(board_html) != ui->board_hash));
    // crosshair settings changed in the Display tab (markup or opacity rules)
    const ClientRenderSettings *rs = client_render_settings();
    char cross_sig[sizeof(ui->cross_sig)], cross_opts[sizeof(ui->cross_opts)];
    hud_cross_sig(rs, cross_sig, sizeof(cross_sig));
    snprintf(cross_opts, sizeof(cross_opts), "%d|%d|%g", rs->crosshair_always, rs->crosshair_use_opacity, rs->crosshair_opacity);
    const int cross_settings_changed = strcmp(cross_sig, ui->cross_sig) != 0 || strcmp(cross_opts, ui->cross_opts) != 0;
    if (cross_settings_changed) snprintf(ui->cross_opts, sizeof(ui->cross_opts), "%s", cross_opts);
    // v3.1.1 LqKL1: the crosshair opacity is also zero while inspecting the weapon
    const int inspecting = me->inspecting && !ov;
    const int cross_changed = force || inspecting != ui->inspecting || ui->cross_dirty || next_extra.spread != ui->extra.spread || next_extra.aim_pct != ui->extra.aim_pct ||
                              reloading != ui->reloading || next_extra.scoreboard != ui->extra.scoreboard || cross_settings_changed;
    const int ping_changed = force || ping != ui->ping;

    if (!(health_changed || ammo_changed || timer_changed || counters_changed || class_changed || scope_changed ||
          loadout_changed || scoreboard_changed || cross_changed || fps_changed || ping_changed)) {
        return;
    }

    *hud = next;
    ui->extra = next_extra;
    ui->reloading = reloading;
    ui->inspecting = inspecting;
    hud->valid = 1;
    hud->next_emit_time = now + HUD_UI_MIN_INTERVAL;

    if (health_changed) hud_render_health(hud);
    if (ammo_changed) hud_render_ammo(hud);
    if (timer_changed) hud_render_timer(hud->timer_text);
    if (counters_changed) {
        dom_text_int("killsVal", hud->kills);
        dom_text_int("deathsVal", hud->deaths);
        dom_text_int("myScoreVal", hud->score);
        // kdVal = (kills / (deaths || 1)).round(1).toString()
        char kd[32];
        const double ratio = (double) hud->kills / (hud->deaths ? hud->deaths : 1);
        fmt_number(round(ratio * 10.0) / 10.0, kd, sizeof(kd));
        if (!hud->kills && !hud->deaths) snprintf(kd, sizeof(kd), "0.0"); // resetGameSessionState: kdVal = "0.0"
        dom_text("kdVal", kd);
    }
    if (fps_changed || force) dom_text_int("ingameFPS", hud->fps);
    int streak = (int) me->kill_streak;
    if (ov) streak = hud_json_int(g_hud_override.hud, "streak", streak); // scenario hud.streak (kn-ingame-topright)
    if (force || streak != ui->streak) { // streakVal = currentKillStreak
        ui->streak = streak;
        dom_text_int("streakVal", ui->streak);
    }
    static int shown_team = -1; // io_updateTeam (isYou): the card's team name follows a team change too
    if (class_changed) {
        hud_render_class(hud->class_index);
        hud_render_team(client);
        shown_team = me->team;
    } else if (force || (int) me->team != shown_team) {
        hud_render_team(client);
        shown_team = me->team;
    }
    if (scope_changed) hud_render_scope(hud->scoped);
    if (aim_dot_changed) dom_style("aimDot", "opacity", hud->aim_dot ? "1" : "0");
    if (ping_changed) {
        hud_render_ping(ping);
        ui->ping = ping;
    }
    if (loadout_changed) hud_render_loadout(&ui->extra, loadout_rebuild);
    if (scoreboard_changed) hud_render_scoreboard(ui->extra.scoreboard, board_html);
    if (cross_changed) hud_render_crosshair(&ui->extra, ui->reloading || ui->inspecting);
    hud_seed_chat(client);
    hud_chat_channel_check(client);

    // TODO: call hud_ui_add_killfeed() when the game exposes kill events (player_kill()
    // only deactivates the victim today) and hud_ui_hit() when hitscan damage lands
    // (shared/src/player.c hitscan is still a TODO).
}

// punishPlayer(id, action) as the list sends it: the server's chat command for the action (kick
// "/kick", ban "/ban", kill, tp2 / tpme / bring, "/bring all" for 9); the server checks the rights
static void plist_punish_send(const int uid, const int action) {
    if (action == 0 || action == 1) { // Kick / Ban: v10 ADMIN_ACTIONS (the server checks DEV and the target)
        if (menu_ui()->client) hud_votekick_admin_action(menu_ui()->client, uid, action);
        return;
    }
    const char *alias = hud_plist_alias(uid);
    char line[NET_CHAT_TEXT_SIZE] = "";
    if (action == 9) snprintf(line, sizeof(line), "/bring all");
    else if (alias) {
        const char *cmd = action == 0 ? "kick" : action == 1 ? "ban" : action == 3 ? "kill" : action == 4 ? "tp" : action == 5 ? "bring" : NULL;
        if (cmd) snprintf(line, sizeof(line), "/%s %s", cmd, alias);
    }
    if (line[0] && menu_ui()->client) client_chat_send(menu_ui()->client, line);
}

// the ban confirm's answer (v10 banPlayerConfirmed: clearPops, punishPlayer(id, 1))
static int g_ban_uid = -1;
static void plist_ban_answered(const int accepted, const char *text, void *user) {
    (void) text;
    (void) user;
    const int uid = g_ban_uid;
    g_ban_uid = -1;
    ui_loadout_clear_pops();
    if (accepted && uid >= 0) plist_punish_send(uid, 1);
}

// Inline handlers this module owns (ui_handlers.c h_fallback); -1 = not ours.
int hud_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    if (!name) return -1;
    if (!strcmp(name, "switchChat")) { // #chatSwitch
        hud_switch_chat();
        return 1;
    }
    if (!strcmp(name, "windows.22.searchPlayers")) { // Player List search box
        char value[sizeof(g_plist_search)] = "";
        if (ev && ev->target) html_dom_get_value(ev->target, value, sizeof(value));
        else if (ev && ev->value) snprintf(value, sizeof(value), "%s", ev->value);
        snprintf(g_plist_search, sizeof(g_plist_search), "%s", value);
        char *rows = hud_plist_rows();
        const HtmlElem holder = html_dom_by_id("playerListH");
        if (holder) html_dom_set_html(holder, rows);
        free(rows);
        return 1;
    }
    if (!strcmp(name, "muteUser")) { // muteUser(this, id, 0 chat / 1 voice): player.accid in or out of the mute list
        const int voice = argc > 2 && args[2].kind == HTML_ARG_NUM && args[2].num != 0.0;
        const HtmlElem btn = ev ? ev->target : 0; // this: the clicked button (events.c leaves HTML_ARG_THIS unresolved)
        int uid = -1;
        if (argc > 1 && args[1].kind == HTML_ARG_NUM) uid = (int) args[1].num;
        else if (argc > 1 && args[1].kind == HTML_ARG_STR && args[1].str) uid = atoi(args[1].str);
        const NetPlayerInfoPacket *info = uid >= 0 ? player_accounts_get(uid) : NULL;
        if (!info || info->account_id <= 0) return 1; // Game.players.findById(id).accid
        const int muted = social_toggle_mute(info->account_id, voice);
        if (btn) html_dom_set_text(btn, voice ? (muted ? "voice_over_off" : "record_voice_over") : (muted ? "speaker_notes_off" : "chat"));
        return 1;
    }
    if (!strcmp(name, "focusPlayer")) { // focusPlayer(id, this): follow / unfollow them, the list redrawn (updateWindow(23))
        const int uid = argc > 0 && args[0].kind == HTML_ARG_STR && args[0].str ? atoi(args[0].str) : argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : -1;
        if (uid >= 0 && menu_ui()->client) spectate_focus_player(menu_ui()->client, uid, 0);
        if (menu_ui()->active_window == 23) menu_show_window(23, 1);
        return 1;
    }
    if (!strcmp(name, "clearFocus")) { // clearFocus(): "Focus Cleared", updateWindow(23)
        spectate_clear_focus_player();
        if (menu_ui()->active_window == 23) menu_show_window(23, 1);
        return 1;
    }
    if (!strcmp(name, "punishPopup")) { // punishPopup(id): v10 genericPopup(..., "confPop")
        const int uid = argc > 0 && args[0].kind == HTML_ARG_STR && args[0].str ? atoi(args[0].str) : argc > 0 ? (int) args[0].num : -1;
        const char *alias = hud_plist_alias(uid);
        if (!alias) return 1;
        int dev = 0, host = 0;
        hud_plist_staff(menu_ui()->client, &dev, &host);
        StrBuf sb = {0};
        char q[96];
        snprintf(q, sizeof(q), "Are you sure you want to take action on %s?", alias); // windows.playerlist.punish
        sb_append(&sb, "<div style='color:#fff;font-size:15px;position:relative;margin-bottom:15px;'>");
        sb_append_esc(&sb, q);
        sb_append(&sb, "</div><div class='takeActionTable'>");
        sb_appendf(&sb, "<span onmouseenter='playTick()' class='takeActionBtn kick' onclick='punishPlayer(\"%d\", 0);clearPops();'>Kick</span>", uid);
        if (dev) sb_appendf(&sb, "<span onmouseenter='playTick()' class='takeActionBtn ban' onclick='banPlayer(\"%d\")'>Ban</span>", uid);
        sb_append(&sb, "</div>");
        char *html = sb_finish(&sb);
        ui_loadout_clear_pops();
        dom_display("popupHolder", "block");
        const HtmlElem pop = html_dom_by_id("genericPop");
        if (pop) {
            html_dom_set_attr(pop, "class", "confPop");
            html_dom_set_attr(pop, "style", "");
            html_dom_set_html(pop, html);
            html_dom_set_display(pop, "block");
        }
        free(html);
        return 1;
    }
    if (!strcmp(name, "punishPlayer")) {
        // punishPlayer(id, action): the server's chat commands, which check the rights themselves
        // (0 kick, 3 kill, 4 TP2 = teleport to them, 5 TPME = bring them, 9 TPAll)
        const int action = argc > 1 ? (int) (args[1].kind == HTML_ARG_STR && args[1].str ? atoi(args[1].str) : args[1].num) : -1;
        const int uid = argc > 0 && args[0].kind == HTML_ARG_STR && args[0].str ? atoi(args[0].str) : argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : -1;
        plist_punish_send(uid, action);
        return 1;
    }
    if (!strcmp(name, "banPlayer")) { // banPlayer(id): v10's YES/NO confirm, then punishPlayer(id, 1)
        const int uid = argc > 0 && args[0].kind == HTML_ARG_STR && args[0].str ? atoi(args[0].str) : argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : -1;
        const char *alias = hud_plist_alias(uid);
        if (!alias) return 1;
        char q[160];
        snprintf(q, sizeof(q), "Are you sure you want ban %s from this game?", alias); // windows.playerlist.ban
        g_ban_uid = uid;
        ui_confirm(q, plist_ban_answered, NULL);
        return 1;
    }
    {
        const int r = hud_votekick_handle(menu_ui()->client, name, ev, args, argc); // kickVote, confirmPolice, callPolice, copyUser
        if (r >= 0) return r;
    }
    return hud_v10_handle(name); // the v10 page's own HUD handlers (hud_v10.c), -1 when not one
}
