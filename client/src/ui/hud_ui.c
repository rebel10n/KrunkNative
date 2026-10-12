// In-game HUD over the DOM-ops API (client/include/html_dom.h), the C port of the
// old ui/js/hud.js (doc/html-engine-plan.md 2.3); no JS runs natively.
//
// client_hud_html_tick() samples the game at 20 Hz (change detection, fps once a
// second) and writes Krunker's #inGameUI markup directly: one DOM write group per
// changed value group. Every write mirrors what
// the real v3.1.1 client does with default settings, verified against the DOM dumps
// in doc/ref/krunker-v3.1.1/ingame/*.html:
//   UI.toggleGameUI/toggleControlUI -> which holders are display:block (hud_apply_visibility)
//   updatePlayerAmmo / reloadUIAnim -> ammoVal/ammoMax, ammoDisplay background-size "N% 100%"
//   healthValue.innerHTML = hp + " <span id='maxHP'>| " + max + "</span>", 6 .healthBarSeg
//   curGameInfo.innerHTML = mode + "<div style='color:rgba(255,255,255,0.6)'>on " + map + "</div>"
//   pingText "-" while the ping is unknown, fpsDisplay hidden, K/D/score counters hidden
//   weapDisplay: one .weapItem per loadout slot ([E]/[Q] keys, 0.7 opacity when holstered)
//   chat: map welMsg + "<name> joined the game" rows (config.serverMessages[0])
//   addKillFeed: <div id='feed_N'><div class='kfItem'><span class='kfMsg'>..</span></div><br>
//   updateLeaderboard (leaderType 1): .newLeaderItem rows in #leaderContainerD while Tab is held
//   game-overlay default crosshair (type 1): four 4x21 bars on a square of
//     max(58, spread * 1.1 * uiHeight / 693), opacity = 1 - aimVal, hidden while reloading
//   game-overlay showHit(): four 7x25 bars at 45deg, spacing 37 (+0.14/ms), fading 0.0062/ms
// Krunker draws the last two on its <canvas id="overlay">; the page builds them from
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
#include <render_settings.h>
#include "../viewer/viewer_store.h" // store.skins: the melee skin icon
#include <streamer.h>
#include <spectate.h>
#include <lobby_ui.h> // WP-E: the end screen's match vote cards (net/lobby_events.c)
#include <cJSON.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define HUD_UI_MIN_INTERVAL 0.0    // HUD sampled every frame like index.js gameLoop; writes stay change-gated
#define HUD_UI_FPS_INTERVAL 0.1    // seconds between #ingameFPS updates (lastFpsDisplayUpdate >= 100)
#define HUD_UI_LOADOUT_MAX 3       // Player.loadout slots mirrored into #weapDisplay

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
#define XP_BAR_COLOR "#fbc02d"     // settings.interface.xpBarCol default
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

static const char *const WEAP_KEYS[HUD_UI_LOADOUT_MAX] = {"", "[E]", "[Q]"}; // swap keys printed next to slots 1/2

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
    const Weapon *loadout[HUD_UI_LOADOUT_MAX];
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
} HudUi;

static HudUi g_hud_ui = {.visible = -1, .in_game = -1, .cross_shown = -1, .ping = INT_MIN, .chat_shown = -1, .board_shown = -1, .old_board = -1, .feed_last_ms = -1.0, .dmg_last_ms = -1.0, .nums_last_ms = -1.0, .speed_shown = -1, .show_ui = -1};

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

// healthValue.innerHTML
static void hud_health_value_html(const int hp, const int max, char *out, const size_t size) {
    snprintf(out, size, "%d <span id='maxHP'>| %d</span>", hp, max);
}

// healthBar.innerHTML: `segs` .healthBarSeg, each filled by the segment's share of hp/max,
// red once at most 3 segments worth of health are left.
static void hud_health_bar_html(int hp, int max, int segs, char *out, const size_t size) {
    hp = hp < 0 ? 0 : hp;
    max = max < 1 ? (hp > 0 ? hp : 1) : max;
    segs = segs < 1 ? 1 : segs;

    const double left = (double) segs * ((double) hp / (double) max); // segments worth of health left
    const char *color = left <= 3.0 ? HEALTH_COLOR_LOW : HEALTH_COLOR_HIGH;
    size_t len = 0;
    out[0] = 0;

    for (int i = 0; i < segs; i++) {
        const double v = left - (double) i;
        char width[32];
        fmt_number((v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v) * 100.0, width, sizeof(width));
        hb_append(out, size, &len, "<div class='healthBarSeg'><div class='hpBSeg' style='width:%s%%;background-color:%s'></div></div>", width, color);
    }
}

// weapDisplay icon: `canThrow && melee` -> the melee skin icon (default knife: melee/icon_0),
// else weaponConfig.icon; a thrown knife (canThrow false) leaves the melee slot on the
// Combat Knife's own icon_0, the fists, until refillKnife / respawn.
static const char *hud_weapon_icon_src(const Weapon *slot, const int can_throw, const int melee_icon, char *out, const size_t size) {
    if (slot && slot->melee && can_throw && melee_icon > 0) {
        char path[64]; // a skin's icon lives with the game assets, not in the page's textures/
        snprintf(path, sizeof(path), "textures/melee/icon_%d.png", melee_icon);
        html_dom_asset_url(path, out, size);
    } else if (slot && slot->melee && can_throw) snprintf(out, size, "textures/melee/icon_0.png");
    else snprintf(out, size, "textures/weapons/%s.png", slot && slot->icon ? slot->icon : "icon_0");
    return out;
}

// weapDisplay.innerHTML (player manager, isYou branch): one .weapItem per slot, the held
// one at full opacity, holstered ones at 0.7 with their swap key.
static void hud_weapon_rack_html(const Weapon *const *loadout, const int count, const int index, const int can_throw, const int melee_icon, char *out,
                                 const size_t size) {
    size_t len = 0;
    out[0] = 0;

    for (int i = 0; i < count && i < HUD_UI_LOADOUT_MAX; i++) {
        char src[256];
        hb_append(out, size, &len, "<div class='weapItem' id='weapItem_%d'>", i);
        if (i) hb_append(out, size, &len, "<div class='weapKey'>%s</div>", WEAP_KEYS[i]);
        hb_append(out, size, &len, "<img style='opacity:%s%s' class='weapIcon' src='%s'%s></div>",
            i == index ? "1" : "0.7", i ? ";margin-right:25px" : "",
            hud_weapon_icon_src(loadout[i], can_throw, melee_icon, src, sizeof(src)),
            loadout[i] && loadout[i]->melee ? " id='weapIconMel'" : "");
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

// Krunker gB() -> gy(): the inner markup of a #chatMsg_N row. A `name` makes a player
// line (fixStr(name) + ": " + message), else a `color` makes a server line (type 2),
// else a plain game line (type 1, .chatMsg default color).
static void hud_chat_row_html(const char *name, const char *msg, const char *color, char *out, const size_t size) {
    size_t len = 0;
    out[0] = 0;
    hb_append(out, size, &len, "<div class='chatItem'>");
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

// updateLeaderboard: one .newLeaderItem (leaderType 1), or .leaderItem in #leaderContainer
// with Old Scoreboard (leaderType 0: g_board_prefix "l"); the Name`M` class marks the
// local player's row.
static const char *g_board_prefix = "newL";

// acc: the player's account (NET_PACKET_PLAYER_INFO from a brain-linked server), NULL for
// guests: premiumT "beenhere" / featured "check_circle" icons and the " [clan]" tag.
static void hud_scoreboard_row_html_acc(char *out, const size_t size, size_t *len, const int rank, const char *name, const int score, const int is_you,
                                        const NetPlayerInfoPacket *acc) {
    char num[32];
    const char *pre = g_board_prefix;
    hud_format_num(score, num, sizeof(num));
    hb_append(out, size, len, "<div class='%seaderItem'><div class='%seaderCounter'>%d.</div>", pre, pre, rank);
    if (acc && acc->account_id > 0 && (acc->flags & NET_PLAYER_INFO_PREMIUM))
        hb_append(out, size, len, " <i class='material-icons' style='color:#FBC02D;margin-top:3px;font-size:23px;vertical-align:middle;'>beenhere</i>");
    if (acc && acc->account_id > 0 && (acc->flags & NET_PLAYER_INFO_VERIFIED))
        hb_append(out, size, len, " <i class='material-icons' style='color:#40C4FF;margin-top:3px;font-size:23px;vertical-align:middle;'>check_circle</i>");
    hb_append(out, size, len, "<div class='%seaderName%s'>", pre, is_you ? "M" : "");
    char shown[PLAYER_NAME_SIZE];
    hb_append_escaped(out, size, len, streamer_name(name && name[0] ? name : "Guest", shown, sizeof(shown))); // streamerModeEnabled ? scrambleS(alias)
    if (acc && acc->account_id > 0 && acc->clan[0]) { // COLORS.getClanCol(clan, clanR, "#fff"): "DEV" is blue
        hb_append(out, size, len, "<span class='' style='color:%s'> [", strcmp(acc->clan, "DEV") ? "#fff" : "#40C4FF");
        hb_append_escaped(out, size, len, acc->clan);
        hb_append(out, size, len, "]</span>");
    }
    hb_append(out, size, len, "</div><div class='%seaderScore'>%s</div></div>", pre, num);
}

static void hud_scoreboard_row_html(char *out, const size_t size, size_t *len, const int rank, const char *name, const int score, const int is_you) {
    hud_scoreboard_row_html_acc(out, size, len, rank, name, score, is_you, NULL);
}

// Scenario rows [{rank, name, score, me?}] (ui_scenario.c `state.scoreboard`).
static void hud_scoreboard_rows_html(const char *my_name, const cJSON *rows, char *out, const size_t size, size_t *len) {
    const int count = cJSON_GetArraySize(rows);
    int index = 0;
    const cJSON *row;
    cJSON_ArrayForEach(row, rows) {
        const cJSON *rank = cJSON_GetObjectItemCaseSensitive(row, "rank");
        const cJSON *row_name = cJSON_GetObjectItemCaseSensitive(row, "name");
        const cJSON *row_score = cJSON_GetObjectItemCaseSensitive(row, "score");
        const cJSON *me = cJSON_GetObjectItemCaseSensitive(row, "me");
        const char *n = cJSON_IsString(row_name) ? row_name->valuestring : "";
        // An FFA board always lists the local player: a single row is them.
        const int is_you = cJSON_IsBool(me) ? cJSON_IsTrue(me) : count == 1 || (my_name && my_name[0] && !strcmp(n, my_name));
        index++;
        hud_scoreboard_row_html(out, size, len, cJSON_IsNumber(rank) ? rank->valueint : index, n,
            cJSON_IsNumber(row_score) ? row_score->valueint : 0, is_you);
    }
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

// #leaderContainerD markup (FFA, leaderType 1): scenario rows when a scenario supplies
// them, else every known player sorted by score (at most LEADER_MAX rows like Krunker),
// else just the local player.
static void hud_scoreboard_html(const Client *client, const cJSON *rows, char *out, const size_t size) {
    size_t len = 0;
    out[0] = 0;
    if (cJSON_IsArray(rows)) {
        hud_scoreboard_rows_html(client->me ? hud_player_name(client, client->me) : client->menu_name, rows, out, size, &len);
        return;
    }
    const Player *players[HUD_BOARD_PLAYERS_MAX];
    const int count = client->game.players ? hud_sorted_players(client, players, HUD_BOARD_PLAYERS_MAX) : 0;
    if (count == 0) {
        hud_scoreboard_row_html(out, size, &len, 1, client->me ? hud_player_name(client, client->me) : client->menu_name,
            client->me ? client->me->score : 0, 1);
        return;
    }
    for (int i = 0; i < count && i < LEADER_MAX; i++) {
        hud_scoreboard_row_html_acc(out, size, &len, i + 1, hud_player_name(client, players[i]), players[i]->score, players[i] == client->me,
                                    player_accounts_get(players[i]->uid));
    }
}

// The " [clan]" tag of a player list row: COLORS.getClanCol(clan, clanR, "#353535") ("DEV"
// is blue; KrunkNative has no clan ranks, so no rainbow / rank colours).
static void hud_plist_clan(StrBuf *sb, const char *clan) {
    if (!clan || !clan[0]) return;
    sb_appendf(sb, "<span class='' style='color:%s'> [", strcmp(clan, "DEV") ? "#353535" : "#40C4FF");
    sb_append_esc(sb, clan);
    sb_append(sb, "]</span>");
}

// windows[22] "Player List" (showWindow(23), opened by pListKey): the header with the
// player count, the local player's row, then every player in Game.players.list as a
// profile link. Not ported, as KrunkNative has no backend for them: the row buttons
// (Kick Spec / Votekick / Kick / Ban / Kill / TP2 / TPME / TPAll / police, which need host or
// staff punish packets and v2k), the ranked elo icon (no per-player maxElo) and isHacker's
// red link (no flag). config.isProd (not a localhost/LAN host) leaves the local player out
// of the list, as on krunker.io; the local player always has the top row.
char *hud_ui_player_list_html(void) {
    const Client *client = menu_ui()->client;
    const size_t count = client && client->game.players ? client->game.player_count : 0;
    size_t listed = 0;
    for (size_t i = 0; i < count; i++) listed += client->game.players[i] != NULL;
    StrBuf sb = {0};
    sb_append(&sb, "<div style='margin-top:0px' class='setHed'>");
    if (listed) sb_appendf(&sb, "Player List<div style='float: right;'>[%zu]</div><hr>", listed); // windows.players.header
    else sb_append(&sb, "<center>No players currently...<center>");                       // windows.players.none
    sb_append(&sb, "</div><div style='background-color:gainsboro;border-radius:6px;padding:5px'>");
    // isLobbyHost: this client runs the game (offline / hosted), like menu_ui.c's idle kick
    const int remote = client && client->net_connected && !client->offline && !client->game.is_local;
    const char *host = client ? client->net_address : "";
    const int prod = remote && host[0] && strcmp(host, "127.0.0.1") && strcmp(host, "localhost") && strncmp(host, "192.168.", 8);
    const Player *me = client ? client->me : NULL;
    if (me) {
        sb_append(&sb, "<div><span class='settNameSmall'>");
        if (!remote) sb_append(&sb, "<i class='material-icons' style='font-size:25px'>stars</i>");
        sb_append_esc(&sb, hud_player_name(client, me));
        const NetPlayerInfoPacket *info = player_accounts_get(me->uid);
        const AccountInfo *acc = account_get();
        hud_plist_clan(&sb, info && info->account_id > 0 ? info->clan : acc ? acc->clan : NULL);
        sb_append(&sb, "</span><span style='float:right'></span></div>");
    }
    for (size_t i = 0; i < count; i++) {
        const Player *p = client->game.players[i];
        if (!p || (prod && p == me)) continue; // Game.players.list.filter(ho => !config.isProd || !ho.isYou)
        const NetPlayerInfoPacket *info = player_accounts_get(p->uid);
        const char *name = hud_player_name(client, p);
        sb_append(&sb, "<div><span class='settNameSmall'><a target='_blank' class='grey' href='/social.html?p=profile&q=");
        sb_append_esc(&sb, name);
        sb_append(&sb, "'>");
        sb_append_esc(&sb, name);
        hud_plist_clan(&sb, info && info->account_id > 0 ? info->clan : NULL);
        sb_append(&sb, "</a></span><span style='float:right'></span></div>");
    }
    sb_append(&sb, "</div>");
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

// weaponConfig icnPad by icon (margin-right of the .weaponChatIcon); icon_0 is the Combat
// Knife, a melee weapon: gE() shows the killer's melee skin icon (default knife = melee
// icon_0, gD[0] = 5 px) as .meleeChatIcon instead.
static const struct {
    const char *icon;
    int pad;
} FEED_ICON_PAD[] = {
    {"icon_1", 9}, {"icon_2", 5}, {"icon_3", -15}, {"icon_4", 0}, {"icon_5", -10}, {"icon_6", 10}, {"icon_7", 10},
    {"icon_8", 10}, {"icon_9", 10}, {"icon_10", -4}, {"icon_11", -10}, {"icon_12", -8}, {"icon_14", 9},
    {"icon_15", 9}, {"icon_16", 10}, {"icon_17", -10}, {"icon_18", 9}, {"icon_19", 5},
};
#define FEED_MELEE_ICON "icon_0"
#define FEED_MELEE_PAD 5           // gD[0]: default knife skin

static int hud_feed_icon_pad(const char *icon) {
    for (size_t i = 0; i < sizeof(FEED_ICON_PAD) / sizeof(FEED_ICON_PAD[0]); i++) {
        if (!strcmp(FEED_ICON_PAD[i].icon, icon)) return FEED_ICON_PAD[i].pad;
    }
    return 0; // gC.icnPad || 0
}

// gE(): "<div class='kfItem'><span class='kfMsg'>" in #killFeed; with Old Scoreboard
// (`chat`) "<div class='chatItem[ chatTextOutline]'[ style='background-color: rgba(0, 0, 0, op)']>
// <span class='chatMsg'>" (gv / gu from chatTextOutline / chatBGOp).
static void hud_feed_open(char *out, const size_t size, size_t *len, const int chat) {
    const ClientRenderSettings *rs = client_render_settings();
    if (!chat) {
        hb_append(out, size, len, "<div class='kfItem'><span class='kfMsg'>");
        return;
    }
    hb_append(out, size, len, "<div class='chatItem%s'", rs->chat_text_outline ? " chatTextOutline" : "");
    if (fabsf(rs->chat_bg_opacity - CHAT_BG_OPACITY_DEFAULT) > 1e-4f)
        hb_append(out, size, len, " style='background-color: rgba(0, 0, 0, %g)'", (double) rs->chat_bg_opacity);
    hb_append(out, size, len, "><span class='chatMsg'>");
}

// gE(b8, info, killer, victim): inner markup of a kill row. `killer` NULL = the victim
// killed itself (hC = ""): only the weapon and the victim.
static void hud_feed_row_html(const int chat, const char *killer, const int killer_is_you, const char *victim,
                              const int victim_is_you, const char *weapon_icon, const int headshot, char *out,
                              const size_t size) {
    size_t len = 0;
    out[0] = 0;
    hud_feed_open(out, size, &len, chat);
    if (killer || killer_is_you) hud_feed_name_span(out, size, &len, killer ? killer : "", killer_is_you, 0);
    if (weapon_icon && !strcmp(weapon_icon, FEED_MELEE_ICON)) {
        hb_append(out, size, &len, "<img style='opacity:0.7;margin-right:%dpx;' class='meleeChatIcon' src='textures/melee/icon_0.png' />",
                  FEED_MELEE_PAD);
    } else if (weapon_icon && weapon_icon[0]) {
        hb_append(out, size, &len, "<img style='opacity:0.7;margin-right:%dpx;' class='weaponChatIcon' src='textures/weapons/",
                  hud_feed_icon_pad(weapon_icon));
        hb_append_escaped(out, size, &len, weapon_icon);
        hb_append(out, size, &len, ".png' />");
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

static void hud_loadout(const Player *me, HudUiExtra *extra) {
    const Game *game = me->game;
    const size_t weapon_count = game->weapon_count ? game->weapon_count : sizeof(g_weapons) / sizeof(g_weapons[0]);
    const Weapon **weapons = game->weapons ? (const Weapon**) game->weapons : (const Weapon**) g_weapons;

    extra->loadout_size = 0;
    extra->loadout_index = me->loadout_index;
    extra->can_throw = me->can_throw;
    const ViewerSkin *melee = viewer_skin(me->skins.melee);
    extra->melee_icon = melee && melee->type == 3 && melee->id > 0 ? melee->id : 0;
    for (int i = 0; i < me->loadout_size && i < HUD_UI_LOADOUT_MAX; i++) {
        const int id = me->loadout ? me->loadout[i] : -1;
        extra->loadout[extra->loadout_size++] = id >= 0 && (size_t) id < weapon_count ? weapons[id] : NULL;
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

static int hud_class_icon_index(const Client *client, const int class_index) {
    const Game *game = &client->game;
    const ClassConfig *class = game->classes && class_index >= 0 && (size_t) class_index < game->class_count ? &game->classes[class_index] : NULL;
    return class ? class->icon_index : class_index;
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
        static const char *const sides[4] = {"t", "r", "b", "l"};
        for (int i = 0; i < 4; i++) {
            hb_append(html, sizeof(html), &len, "<div class='knCrossArm knCrossArm-%s' style='background-color:%s;box-shadow:", sides[i], color);
            if (shadow_on) hb_append(html, sizeof(html), &len, "0 0 0 2px %s'></div>", shadow);
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
    hud_ui_add_killfeed_ex(client, killer, killer_is_you, victim, victim_is_you, weapon_icon, is_headshot, 0.0);
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

void hud_ui_add_killfeed_ex(Client *client, const char *killer, const int killer_is_you, const char *victim, const int victim_is_you,
                            const char *weapon_icon, const int is_headshot, const double age_ms) {
    (void) client;
    if (!client_render_settings()->show_kills) return; // bf.tmp.showKillFeed
    const int chat = hud_feed_in_chat();
    char html[1024];
    hud_feed_row_html(chat, killer, killer_is_you, victim ? victim : "", victim_is_you, weapon_icon, is_headshot, html, sizeof(html));
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
    char html[768];
    size_t len = 0;
    html[0] = 0;
    hud_feed_open(html, sizeof(html), &len, 1);
    if (player == client->me) {
        hb_append(html, sizeof(html), &len, "<span style='color:" FEED_COLOR_YOU "'>You're</span>");
    } else {
        const Player *me = client->me;
        const int ally = me && me->team && client->game.mode && client->game.mode->config.teams && me->team == player->team;
        hud_feed_name_span(html, sizeof(html), &len, hud_player_name(client, player), 0, ally);
        hb_append(html, sizeof(html), &len, " is");
    }
    hb_append(html, sizeof(html), &len, " on a %d Kill Streak</span></div><br>", streak);
    hud_feed_insert(1, html, 0.0);
}

/* ------------------------------------------------------------ chat */

// gy(): appends one #chatMsg_N row (see hud_chat_row_html for the three kinds).
static void hud_add_chat_row(const char *name, const char *msg, const char *color) {
    const HtmlElem list = html_dom_by_id("chatList");
    if (!list || !msg || !msg[0]) return;

    char id[32], html[1024];
    snprintf(id, sizeof(id), "chatMsg_%d", g_hud_ui.chat_seq++);
    hud_chat_row_html(name, msg, color, html, sizeof(html));

    const HtmlElem row = html_dom_create("div", id);
    html_dom_set_html(row, html);
    html_dom_append(list, row);
    html_dom_scroll_to_bottom(list);
}

// addChat(msg) for a server line.
static void hud_add_server_chat(const char *msg, const char *color) {
    hud_add_chat_row(NULL, msg, color);
}

void hud_ui_add_chat(const char *name, const char *msg, const char *color) {
    if (!html_ui_available() || !html_ui_is_ready()) return;
    hud_add_chat_row(name, msg, color);
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

    if ((el = html_dom_by_id("healthValue"))) {
        hud_health_value_html(hud->health < 0 ? 0 : hud->health, hud->max_health < 1 ? 1 : hud->max_health, html, sizeof(html));
        html_dom_set_html(el, html);
    }
    if ((el = html_dom_by_id("healthBar"))) {
        hud_health_bar_html(hud->health, hud->max_health, hud->health_segments, html, sizeof(html));
        html_dom_set_html(el, html);
    }
}

static void hud_render_ammo(const HtmlHudState *hud) {
    char text[32];

    if (hud->ammo) snprintf(text, sizeof(text), "%d", hud->ammo);
    else snprintf(text, sizeof(text), "%s", hud->melee ? "-" : "0");
    dom_text("ammoVal", text);

    if (hud->ammo_limit || hud->ammo_max <= 0) snprintf(text, sizeof(text), "| -");
    else snprintf(text, sizeof(text), "| %d", hud->ammo_max);
    dom_text("ammoMax", text);

    // reloadUIAnim: ammoDisplay.style.backgroundSize = Math.round(pct) + "%  100%" (95.js), so the
    // blank_2 fill keeps the full box height and only its width drains right-to-left. A bare
    // "N%" / "N% auto" would scale the square image's height too and shrink it into the top-left
    // corner. resetReloadUIAnim writes "0%" once the reload ends.
    if (hud->reload_percent > 0) snprintf(text, sizeof(text), "%d%% 100%%", hud->reload_percent);
    else snprintf(text, sizeof(text), "0%%");
    dom_style("ammoDisplay", "background-size", text);

    const int need_reload = !hud->melee && hud->ammo_max > 0 && hud->ammo <= 0 && !hud->ammo_limit;
    dom_display("reloadMsg", need_reload ? "block" : "none");
}

static void hud_render_timer(const char *timer_text) {
    dom_text("timerVal", timer_text);
    const int hidden = !timer_text[0] || strcmp(timer_text, "0:00") == 0 || strcmp(timer_text, "00:00") == 0;
    dom_display("timerDisplay", hidden ? "none" : "inline-block");
}

static void hud_render_class(const int icon_index) {
    const HtmlElem img = html_dom_by_id("hudClassImg");
    if (!img) return;

    char src[64];
    snprintf(src, sizeof(src), "textures/classes/icon_%d.png", icon_index);
    html_dom_set_attr(img, "src", src);
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

// reconcilePlayer: pingText = ping == -1 ? "-" : ping; icon colored by COLORS.ping.
static void hud_render_ping(const int ping) {
    char text[16];
    if (ping < 0) snprintf(text, sizeof(text), "-");
    else snprintf(text, sizeof(text), "%d", ping);
    dom_text("pingText", text);
    dom_style("pingIcon", "color", ping >= 150 ? PING_COLOR_HIGH : ping >= 50 ? PING_COLOR_MEDIUM : PING_COLOR_LOW);
}

// The weapon rack bottom-right: rebuilt when the list changes, only the icon opacities
// when the held slot changes.
static void hud_render_loadout(const HudUiExtra *extra, const int rebuild) {
    const HtmlElem host = html_dom_by_id("weapDisplay");
    if (!host) return;

    if (rebuild) {
        char html[2048];
        hud_weapon_rack_html(extra->loadout, extra->loadout_size, extra->loadout_index, extra->can_throw, extra->melee_icon, html, sizeof(html));
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

        HtmlElem img = html_dom_first_child(item); // the <img> is the item's last child
        for (HtmlElem next = img; next; next = html_dom_next(next)) img = next;
        if (img) html_dom_set_style(img, "opacity", i == extra->loadout_index ? "1" : "0.7");
        if (img && throw_changed && extra->loadout[i] && extra->loadout[i]->melee) {
            char src[256];
            html_dom_set_attr(img, "src", hud_weapon_icon_src(extra->loadout[i], extra->can_throw, extra->melee_icon, src, sizeof(src)));
        }
    }
}

// curGameInfo (top-left, under the timer).
static void hud_render_game_info(const Client *client) {
    const HtmlElem info = html_dom_by_id("curGameInfo");
    const char *mode = hud_mode_name(client);
    if (!info || !mode[0]) return;

    char html[512];
    hud_game_info_html(mode, hud_map_name(client), html, sizeof(html));
    html_dom_set_html(info, html);
    html_dom_set_display(info, "");
}

// Old Scoreboard (leaderType 0): g5() keeps #leaderContainer (inside #leaderDisplay, shown
// by oldScoreboard.set) filled with .leaderItem rows, "Empty Lobby" without players;
// switchLeaderboard(0) sets .topRightCounters to 400px wide (auto for leaderType 1).
static void hud_old_board_step(const Client *client) {
    HudUi *ui = &g_hud_ui;
    const int old = client_render_settings()->old_scoreboard;
    if (old != ui->old_board) {
        const HtmlElem counters = html_dom_query(0, ".topRightCounters"); // getElementsByClassName(..)[0]
        if (counters) html_dom_set_style(counters, "width", old ? "400px" : "auto");
        ui->old_board = old;
        ui->old_board_hash = 0;
    }
    if (!old) return;
    char html[4096];
    g_board_prefix = "l";
    hud_scoreboard_html(client, g_hud_override.scoreboard, html, sizeof(html));
    g_board_prefix = "newL";
    if (!html[0]) snprintf(html, sizeof(html), "Empty Lobby"); // ab.t("leaderboard.empty")
    const unsigned hash = hud_hash(html);
    if (hash == ui->old_board_hash) return;
    const HtmlElem c = html_dom_by_id("leaderContainer");
    if (c) html_dom_set_html(c, html);
    ui->old_board_hash = hash;
}

// `html` is the hud_scoreboard_html() markup (ignored while hidden).
static void hud_render_scoreboard(const int shown, const char *html) {
    HudUi *ui = &g_hud_ui;
    if (!html_dom_by_id("newLeaderDisplay")) return;

    if (shown) {
        const unsigned hash = hud_hash(html);
        const HtmlElem d = html_dom_by_id("leaderContainerD");
        if (d && (ui->board_shown != 1 || hash != ui->board_hash)) {
            html_dom_set_html(d, html);
            html_dom_set_display(d, "block");
            ui->board_hash = hash;
        }
        if (ui->board_shown != 1) {
            dom_display("leaderContainer0", "none");
            dom_display("leaderContainer1", "none");
        }
    }
    if (shown != ui->board_shown) dom_display("newLeaderDisplay", shown ? "inline-block" : "none");
    ui->board_shown = shown;
}

/* ------------------------------------------------------------ death card / end screen */

static void hud_toggle_game_ui(int on);
static void hud_end_screen_dom(void);

static int g_end_result;        // fq(): 1 victory, 0 defeat, -1 draw
static char g_end_timer[32];    // fD(time, 1): "Next Round in <time>"

// UI.toggleGameUI(j) with window.Bf5 (spectating) off: the holders the death card swaps
// out (j = false) and back (j = true).
static void hud_toggle_game_ui(const int on) {
    const char *block = on ? "block" : "none";
    dom_display("spectateUI", "none");
    dom_display("mobileUI", "none");
    dom_display("gameMessage", block);
    dom_display("topLeftHolder", block);
    dom_display("topRight", block);
    if (!on) dom_display("reloadMsg", "none"); // on: hud_render_ammo decides (valid = 0 rewrites it)
    dom_display("bottomLeftHolder", block);
    dom_display("weapDisplay", block);
    dom_display("bottomRight", block);
    dom_display("killCardHolder", on ? "none" : "block");
    dom_style("speakerDisplay", "right", on ? "380px" : "20px");
    dom_style("chatHolder", "bottom", on ? "148px" : "20px");
    dom_style("voiceDisplay", "bottom", on ? "145px" : "18px");
    dom_display("overlay", block);
    dom_display("vignette", block);
    dom_display("igXPBar", block);
    dom_display("interactProg", "none");
    dom_display("interactMsg", "none");
    const HtmlElem base = html_dom_by_id("uiBase");
    if (base) html_dom_set_class(base, "onGame", on);
    g_hud_ui.in_game = on;
    // Krunker's crosshair and hit marker live on the overlay canvas hidden above.
    if (!on) {
        hud_hide_crosshair();
        hud_hide_hit();
        hud_dmg_hide_all();
    }
}

// Krunker g1() for the local player: #killCard markup + toggleGameUI(false). `card` NULL
// (or no killer) is the "You died" variant without a card.
void hud_ui_show_death(Client *client, const HudKillCard *card) {
    if (!html_ui_available() || !html_ui_is_ready()) return;
    hud_reset_aim(client); // kill(): toggleAim(player, 0) for the local player
    const int by = card && card->killer && card->killer[0];
    char html[1024];
    size_t len = 0;
    html[0] = 0;
    if (by) {
        hb_append(html, sizeof(html), &len, "<img id='kCProfile' src='textures/classes/icon_%d.png' />", card->class_index);
        hb_append(html, sizeof(html), &len, "<div class='kClvl'>lvl %d</div>", card->level > 1 ? card->level : 1);
        hb_append(html, sizeof(html), &len, "<div class='kCFlag'></div><div id='kCName'>");
        char shown[PLAYER_NAME_SIZE];
        hb_append_escaped(html, sizeof(html), &len, streamer_name(card->killer, shown, sizeof(shown))); // scrambleS(alias) in streamer mode
        hb_append(html, sizeof(html), &len, "</div><br/><div id='kCInfo' style='color:rgba(255,255,255,0.7)'>[");
        hb_append_escaped(html, sizeof(html), &len, card->weapon ? card->weapon : "");
        hb_append(html, sizeof(html), &len, "]</div>");
    }
    if (card && card->score) hb_append(html, sizeof(html), &len, "<div id='kCInfoS' style='color:%s;'>+%d</div>", SCORE_COLOR, card->score);

    dom_display("reloadMsg", "none");
    dom_text("killCardBy", by ? "Killed by" : "You died");
    dom_style("killCardBy", "float", by ? "left" : "none");
    dom_display("killCard", by ? "inline-block" : "none");
    dom_display("killCardKick", "none"); // votekick/report prompt: not offered in KrunkNative
    const HtmlElem kc = html_dom_by_id("killCard");
    if (kc) html_dom_set_html(kc, html);

    g_hud_ui.dead = 1;
    hud_toggle_game_ui(0);
}

// Respawn: toggleGameUI(true).
void hud_ui_hide_death(Client *client) {
    if (!g_hud_ui.dead) return;
    g_hud_ui.dead = 0;
    hud_dmg_clear();
    g_hud_ui.speed_max = 0;   // aV.Qp9.max = 0
    g_hud_ui.speed_cur = -1;
    hud_toggle_game_ui(1);
    client->hud_html.valid = 0; // rewrite every value group (and the crosshair)
    g_hud_ui.cross_shown = -1;
}

// UI.showEndScreen(false) -> UI.hideUI(false) (incl. toggleMenu(false)), fq()'s
// #victorySub and fD(time, 1)'s #endTimer.
static void hud_end_screen_dom(void) {
    // showEndScreen
    dom_display("endMidHolder", "none");
    dom_display("matchVoteHolder", "none");
    dom_display("teamTotal0", "none");
    dom_display("teamTotal1", "none");
    dom_display("endInfo", "none");
    dom_display("endUI", "");
    // hideUI(false) -> toggleMenu(false) + toggleMenuHider(false)
    dom_display("menuHolder", "none");
    dom_display("speakerDisplay", "block");
    dom_display("supportMapCreator", "none");
    dom_display("menuHider", "none");
    const HtmlElem base = html_dom_by_id("uiBase");
    if (base) html_dom_set_class(base, "onMenu", 0);
    dom_display("gameNameHolder", "none");
    dom_display("spectButton", "none");
    html_dom_set_class(html_dom_root(), "kn-hud-visible", 0); // its !important rule would keep #inGameUI up (ui/css/hud.css)
    dom_display("inGameUI", "none");
    dom_display("overlay", "none");
    dom_display("vignette", "none");
    dom_display("spectateUI", "none");
    dom_display("aimRecticle", "none");
    dom_display("instructionHolder", "none");
    dom_display("baseLinks", "none");
    dom_style("chatHolder", "bottom", "20px");
    dom_style("voiceDisplay", "bottom", "18px");
    dom_style("speakerDisplay", "right", "20px");
    dom_display("purchaseHolder", "none");
    dom_display("consentBlock", "none");
    dom_display("bdayBlock", "none");
    // fq(): the title image; shown by fu() after the end animation (the animated title
    // itself is aV.vicTitle on the overlay canvas, #knVicTitle here)
    const HtmlElem sub = html_dom_by_id("victorySub");
    if (sub) {
        html_dom_set_attr(sub, "src", g_end_result < 0 ? "./img/dra.png" : g_end_result ? "./img/vic.png" : "./img/def.png");
        html_dom_set_display(sub, g_hud_ui.end_phase == 2 ? "" : "none");
    }
    if (g_hud_ui.end_phase == 2) {
        dom_display("endInfo", "block");
        dom_display("endMidHolder", "inline-block");
    }
    // fD(time, 1)
    if (g_end_timer[0]) {
        char text[64];
        snprintf(text, sizeof(text), "Next Round in %s", g_end_timer); // i18n timer.end
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

// #knVicTitle: aV.vicTitle on the #game-overlay canvas (below #uiBase): a full-screen
// rgba(0,0,0,.33) fill and img/<vic|def|dra>.png centred at its natural size x scale, both
// at the title opacity. Created next to #game-overlay so it stacks like the canvas.
static HtmlElem hud_end_title(void) {
    HudUi *ui = &g_hud_ui;
    if (ui->end_title) return ui->end_title;
    HtmlElem el = html_dom_by_id("knVicTitle");
    if (!el) {
        const HtmlElem canvas = html_dom_by_id("game-overlay");
        if (!canvas) return 0;
        html_dom_insert_html(canvas, HTML_INSERT_AFTER_END,
            "<div id='knVicTitle' style='position:absolute;left:0;top:0;width:100%;height:100%;z-index:1;pointer-events:none;"
            "background-color:rgba(0,0,0,0.33);display:none'><img id='knVicTitleImg' style='position:absolute;left:50%;top:50%;"
            "transform:translate(-50%,-50%)'></div>");
        el = html_dom_by_id("knVicTitle");
    }
    ui->end_title = el;
    ui->end_title_key[0] = 0;
    return el;
}

// The overlay's vicTitle integration (per frame, bp = dt / 16) replayed in 16 ms steps up
// to `elapsed_ms`, so any clock (scenario or live) gives the same state.
static void hud_end_title_state(const double elapsed_ms, double *opacity, double *scale) {
    double o = 0.0, sc = 2.5, dec = 0.01; // the reset branch leaves decrement at 0.01
    for (double t = 0.0; t < elapsed_ms && t < END_ANIM_MS; t += 16.0) {
        o += 0.05;
        sc -= dec;
        sc = sc > 1.0 ? sc : 1.0;
        dec += 0.01;
    }
    *opacity = o < 1.0 ? o : 1.0;
    *scale = sc;
}

static void hud_end_title_hide(void) {
    HudUi *ui = &g_hud_ui;
    if (ui->end_title) html_dom_set_display(ui->end_title, "none");
    ui->end_title_key[0] = 0;
}

static void hud_end_title_draw(const double elapsed_ms) {
    HudUi *ui = &g_hud_ui;
    const HtmlElem title = hud_end_title();
    if (!title) return;

    double opacity, scale;
    hud_end_title_state(elapsed_ms, &opacity, &scale);
    // overlay px -> CSS px: the canvas is scaled by c7 = innerHeight / #uiBase height
    float vw = 0.0f, vh = 0.0f, dpr = 1.0f;
    html_dom_viewport(&vw, &vh, &dpr);
    const float base_h = hud_ui_height();
    const double c7 = base_h > 0.0f && vh > 0.0f ? vh / base_h : 1.0;
    const char *src = g_end_result < 0 ? "./img/dra.png" : g_end_result ? "./img/vic.png" : "./img/def.png";
    char key[64], text[48];
    snprintf(key, sizeof(key), "%.3f|%.3f|%.4f|%s", opacity, scale, c7, src);
    if (!strcmp(key, ui->end_title_key)) return;
    snprintf(ui->end_title_key, sizeof(ui->end_title_key), "%s", key);

    const HtmlElem img = html_dom_by_id("knVicTitleImg");
    if (img) {
        char current[32];
        if (html_dom_get_attr(img, "src", current, sizeof(current)) < 0 || strcmp(current, src)) html_dom_set_attr(img, "src", src);
        snprintf(text, sizeof(text), "%.2fpx", END_TITLE_W * scale * c7);
        html_dom_set_style(img, "width", text);
        snprintf(text, sizeof(text), "%.2fpx", END_TITLE_H * scale * c7);
        html_dom_set_style(img, "height", text);
    }
    fmt_opacity(opacity, text, sizeof(text));
    html_dom_set_style(title, "opacity", text);
    html_dom_set_display(title, "block");
}

// GUI.endScreen (67.js): tab ids/names in their v3.1.1 order.
static const char *const END_TABS[4][2] = {{"leaderboard", "Leaderboard"}, {"performance", "Performance"}, {"clanwar", "Clan Wars"}, {"medals", "Medals"}};
static int g_end_tab;                 // GUI.endScreen.currentTab (index into END_TABS)
static const Client *g_end_client;    // the client the results describe (tab switches re-render)

// config.endForm.reward: "<span style='color:#F8C55C'>+N</span> KR", "NONE" without one (the
// server's endData.ed cell, shown as is by showEndTable and as the performance tab's funds).
static const char *hud_reward_html(const int reward, char *out, const size_t size) {
    if (reward) snprintf(out, size, "<span style='color:#F8C55C'>+%d</span> KR", reward);
    else snprintf(out, size, "NONE");
    return out;
}

// One showEndTable row: rank, the name link (colour per local / team / enemy), score,
// kills, deaths, the endForm.reward cell and the empty actions cell (no account: no buttons).
static void hud_end_row_html(char *out, const size_t size, size_t *len, const int rank, const int last, const char *name,
                             const char *color, const int score, const int kills, const int deaths, const int reward) {
    hb_append(out, size, len, "<tr style='%s'><td style='color:#fff'>%d.</td><td><a class='endTableN' href='./social.html?p=profile&q=",
        last ? "border-bottom:none" : "", rank);
    hb_append_escaped(out, size, len, name);
    hb_append(out, size, len, "' target='_blank' style='color:%s'>", color);
    char shown[PLAYER_NAME_SIZE];
    hb_append_escaped(out, size, len, streamer_name(name, shown, sizeof(shown))); // showEndTable: scrambleS(alias) text, real link
    char cell[96];
    hb_append(out, size, len, "</a></td><td>%d</td><td>%d</td><td>%d</td><td>%s</td>", score, kills, deaths,
              hud_reward_html(reward, cell, sizeof(cell)));
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

// tabs.leaderboard: <table id="endTable"> filled like fu()'s showTable callback
// (endStats sid, name, score, kills, deaths, reward).
static void hud_end_leaderboard_html(const Client *client, char *out, const size_t size, size_t *len) {
    hb_append(out, size, len, "<table id=\"endTable\">");
    hb_append(out, size, len, "<tr><th>Rank</th><th>Name</th><th>Score</th><th>Kills</th><th>Deaths</th><th>Reward</th><th></th></tr>");
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
        // showEndTable: the local row #fff, a team mate (localTeam == team) rgba(255,255,255,0.6),
        // else COLORS.teams[1]. localTeam = player.team || (spectating ? 1 : 0); a teamless
        // (FFA) player's team is null there, so `0 == null` makes every other FFA row red.
        const int local_team = client->me && client->me->team ? client->me->team : spectate_active() ? 1 : 0;
        const char *color = p == client->me ? "#fff" : p->team && p->team == local_team ? "rgba(255,255,255,0.6)" : FEED_COLOR_ENEMY;
        hud_end_row_html(out, size, len, i + 1, i + 1 >= count, hud_player_name(client, p), color, p->score, (int) p->kills,
                         (int) p->deaths, p->reward);
    }
    hb_append(out, size, len, "</table>");
}

// tabs.performance.gen() for a guest (no account: no XP bar; W/L from this match).
// stats = fu()'s hp: rank "#N", score, kills, deaths, funds ("NONE" without a reward), victory.
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
    const int q = g_end_result;
    const char *mode = hud_mode_name(client)[0] ? hud_mode_name(client) : "Free for All";

    hb_append(out, size, len, "<div id='endLayout'><div><div id='gameFundsLayout'><div class='gfSection'>");
    hb_append(out, size, len, "<div class=\"endTitle\" style=\"text-align:center\">");
    hb_append_escaped(out, size, len, mode);
    // mode.getIcon(): ffa is one of the known ids (img/modes/ffa.png)
    hb_append(out, size, len, "</div> <div id=\"modeLayout\"><img src=\"img/modes/ffa.png\" class=\"modeIcon\"> <div class=\"modeStatus\" style=\"%s\"> %s</div></div>",
        q == -1 ? "color:var(--yellow)" : q ? "" : "color:var(--red)", q == -1 ? "DRAW" : q ? "VICTORY" : "DEFEAT");
    char reward[96];
    hb_append(out, size, len, "</div><div class='gfSection'><div id=\"fundsLayout\" style=\"\"> <div class=\"fundsVal\">%s</div></div></div></div></div>",
              hud_reward_html(me ? me->reward : 0, reward, sizeof(reward))); // fu(): funds = the reward cell
    hb_append(out, size, len, "<div class='endSection'><div class='endTitle'>Combat Record</div><div id='combatRecordLayout'>");
    hb_append(out, size, len, "<div class='crStat'><div class='crLabel'>Score</div><div class='crVal'>%d</div></div>", score);
    hb_append(out, size, len, "<div id='kdLayout'><div class='crStat'><div class='crLabel'>Kills</div><div class='crVal'>%d</div></div>", kills);
    hb_append(out, size, len, "<div class='crStat'><div class='crLabel'>Deaths</div><div class='crVal'>%d</div></div></div>", deaths);
    hb_append(out, size, len, "<div class='crStat'><div class='crLabel'>Rank</div><div class='crVal'>#%d</div></div>", rank);
    hb_append(out, size, len, "<div class='crStat'><div class='crLabel'>SPK</div><div class='crVal'>%.2f</div></div>", (double) score / (kills ? kills : 1));
    hb_append(out, size, len, "<div class='crStat'><div class='crLabel'>KDR</div><div class='crVal'>%.2f</div></div>", (double) kills / (deaths ? deaths : 1));
    hb_append(out, size, len, "<div class='crStat'><div class='crLabel'>W/L</div><div class='crVal'>%.2f</div></div>", q == -1 ? 0.0 : q ? 1.0 : 0.0);
    hb_append(out, size, len, "</div></div></div>");
}

// GUI.endScreen.show(): .endCard with the tab headers (not spectating) and the current tab.
static void hud_end_card_html(const Client *client, char *out, const size_t size) {
    size_t len = 0;
    out[0] = 0;
    // spectators (window.Bf5 / no player: fu()'s ho = true) get the leaderboard without headers
    const int spectator = spectate_active() || !client->me;
    hb_append(out, size, &len, "<div class=\"endCard\">");
    if (!spectator) {
        hb_append(out, size, &len, "<div id='tabHolder'>");
        for (int i = 0; i < 4; i++) {
            hb_append(out, size, &len, "<div class=\"tabHeader %s\" onmouseenter=\"playTick()\" onclick=\"GUI.endScreen.changeTab('%s')\">%s</div>",
                i == g_end_tab ? "selected" : "", END_TABS[i][0], END_TABS[i][1]);
        }
        hb_append(out, size, &len, "</div>");
    } else {
        g_end_tab = 0;
    }
    hb_append(out, size, &len, " <div class=\"tabBody\">");
    switch (g_end_tab) {
        case 1:
            hud_end_performance_html(client, out, size, &len);
            break;
        case 2: // tabs.clanwar: not a custom match and no account -> the login prompt
            hb_append(out, size, &len, "<div style='text-align:center;color:#fff;font-size:30px;background-color:rgba(0, 0, 0, 0.4);border-radius:6px;padding:20px;'>Login to view Clan Wars</div>");
            break;
        case 3: // tabs.medals
            hb_append(out, size, &len, "<div style=\"text-align:center;color:#fff;font-size:30px;background-color:rgba(0, 0, 0, 0.4); border-radius:6px;padding:20px;\">Coming Soon!</div>");
            break;
        default:
            hud_end_leaderboard_html(client, out, size, &len);
            break;
    }
    hb_append(out, size, &len, "</div></div>");
}

static void hud_end_card_render(const Client *client) {
    char *html = malloc(16384);
    if (!html) return;
    hud_end_card_html(client, html, 16384);
    const HtmlElem view = html_dom_by_id("endTabbedView");
    if (view) html_dom_set_html(view, html);
    free(html);
}

// GUI.endScreen.changeTab(id) (the tab headers' onclick).
void hud_ui_end_screen_tab(const char *tab) {
    for (int i = 0; tab && i < 4; i++) {
        if (strcmp(tab, END_TABS[i][0]) != 0) continue;
        g_end_tab = i;
        if (g_hud_ui.ended && g_hud_ui.end_phase == 2 && g_end_client) hud_end_card_render(g_end_client);
        return;
    }
}

// fu(): the results after config.endAnim.
static void hud_end_results(const Client *client) {
    dom_display("endInfo", "block");
    dom_style("endUI", "background-color", "transparent");
    dom_display("victorySub", "");
    hud_end_title_hide();                       // aV.vicTitle.show = false
    dom_display("endAHolderR", "inline-block");
    dom_style("endAHolderR", "opacity", "1");
    dom_display("endAHolderL", "inline-block");
    dom_style("endAHolderL", "opacity", "1");
    dom_display("teamTotal0", "none");          // FFA
    dom_display("teamTotal1", "none");
    if (!lobby_vote_show()) dom_style("endMidHolder", "top", "205px"); // WP-E: f1915 endData.mts cards (lobby_events.c)
    dom_display("endMidHolder", "inline-block");

    g_end_client = client;
    g_end_tab = 0; // GUI.endScreen.init(): changeTab("leaderboard")
    hud_end_card_render(client);
}

// Per frame while the end screen is up: animate the title, then switch to the results.
static void hud_end_step(const Client *client, const double now_ms) {
    HudUi *ui = &g_hud_ui;
    if (!ui->ended || !ui->end_phase) return;
    const double elapsed = now_ms - ui->end_start_ms;
    if (ui->end_phase == 1 && elapsed >= END_ANIM_MS) {
        ui->end_phase = 2;
        hud_end_results(client);
        return;
    }
    if (ui->end_phase == 1 && html_ui_is_menu_open()) hud_end_title_hide(); // never under a menu
    else if (ui->end_phase == 1) hud_end_title_draw(elapsed > 0.0 ? elapsed : 0.0);
}

void hud_ui_show_end_screen(Client *client, const int result, const char *next_round) {
    if (!html_ui_available() || !html_ui_is_ready()) return;
    HudUi *ui = &g_hud_ui;
    g_end_result = result;
    snprintf(g_end_timer, sizeof(g_end_timer), "%s", next_round ? next_round : "");
    if (!ui->ended) {
        // the END: showEndScreen + vicTitle.show; later calls only refresh the countdown.
        // hideUI(false) -> toggleMenu(false): a menu open at the END goes (UI.toggleWindow(false)
        // first); the pointer stays as it was, like v3.1.1's CONTROLS.
        if (html_ui_is_menu_open()) {
            html_ui_set_menu_open(0);
            menu_apply_open(0);
        }
        ui->ended = 1;
        ui->end_phase = 1;
        ui->end_start_ms = hud_now_ms(client);
        dom_style("endUI", "background-color", "");
        hud_end_screen_dom();
        hud_end_step(client, ui->end_start_ms);
        return;
    }
    char text[64];
    snprintf(text, sizeof(text), "Next Round in %s", g_end_timer); // i18n timer.end
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
        client_set_menu_open(client, 0);
        glfwSetInputMode(client->window, GLFW_CURSOR, GLFW_CURSOR_NORMAL); // CONTROLS stay disabled
        client->mouse_state.locked = 0;
        dom_display("endUI", "");
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
    g_end_timer[0] = 0;
    if (!html_ui_available() || !html_ui_is_ready()) return;

    dom_display("endUI", "none");
    dom_display("endInfo", "none");
    dom_display("endMidHolder", "none");
    dom_display("victorySub", "none");
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
    const size_t count = game->weapon_count ? game->weapon_count : sizeof(g_weapons) / sizeof(g_weapons[0]);
    const Weapon *const *weapons = game->weapons ? (const Weapon *const *) game->weapons : (const Weapon *const *) g_weapons;
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
            if (extra->loadout_size >= HUD_UI_LOADOUT_MAX) break;
            const cJSON *name = cJSON_GetObjectItemCaseSensitive(slot, "name");
            extra->loadout[extra->loadout_size++] = hud_weapon_by_name(&client->game, cJSON_IsString(name) ? name->valuestring : NULL);
        }
    }
    extra->loadout_index = hud_json_int(h, "loadoutIndex", extra->loadout_index);
    extra->can_throw = hud_json_int(h, "canThrow", extra->can_throw);
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

// Mirrors UI.toggleGameUI(true) + toggleControlUI(true) with Krunker's default
// settings (DOM dump of a live match, FFA, guest account).
static void hud_apply_visibility(Client *client, const int in_game, const int visible) {
    HudUi *ui = &g_hud_ui;
    HtmlElem el;

    const int on_game = in_game && !ui->dead; // toggleGameUI(false) drops .onGame while dead
    if (on_game != ui->in_game) {
        ui->in_game = on_game;
        if ((el = html_dom_by_id("uiBase"))) html_dom_set_class(el, "onGame", on_game);
    }
    // #chatUI stays up for the whole session (Krunker shows the chat beside the ESC menu,
    // the death card and the end screen); only the menu before the first spawn has none.
    if (in_game != ui->chat_shown) {
        ui->chat_shown = in_game;
        dom_display("chatUI", in_game ? "block" : "none");
    }
    // showUI off (UI.hideGameUI): toggleControlUI keeps #inGameUI, #overlay and #vignette
    // hidden; #aimRecticle still follows the game. A live change re-applies everything.
    const int show_ui = client_render_settings()->show_ui;
    if (show_ui != ui->show_ui) {
        ui->show_ui = show_ui;
        ui->visible = -1;
    }
    // A map / mode change with no hidden frame in between (the offline sandbox reloads and
    // respawns inside one frame, net/offline.c): curGameInfo follows it.
    static int info_key = -1;
    const int key = client->game.ready ? client->game.current_map_index * 1024 + game_mode_id(&client->game) + 1 : -1;
    if (key != info_key) {
        info_key = key;
        if (visible && visible == ui->visible) hud_render_game_info(client);
    }
    if (visible == ui->visible) return;
    ui->visible = visible;

    html_dom_set_class(html_dom_root(), "kn-hud-visible", visible && show_ui); // its !important rule would override showUI

    const char *block = visible ? "block" : "none";
    const char *game_block = visible && show_ui ? "block" : "none";
    dom_display("inGameUI", game_block);
    dom_display("aimRecticle", block);
    dom_display("overlay", game_block);
    dom_display("vignette", game_block);
    dom_display("gameMessage", block);
    dom_display("topLeftHolder", block);
    dom_display("topRight", block);
    dom_display("bottomLeftHolder", block);
    dom_display("botRightHider", block);
    dom_display("weapDisplay", block);
    dom_display("bottomRight", block);
    dom_display("igXPBar", block);
    dom_display("killCardHolder", "none");

    // Pieces the Display settings own (134.js `set`s; ui_render_settings.c applies live
    // changes) are written from the same values, so this pass never undoes a setting.
    const ClientRenderSettings *rs = client_render_settings();
    dom_display("leaderDisplay", rs->old_scoreboard ? "block" : "none");   // oldScoreboard
    dom_display("killCount", rs->show_kill_c ? "inline-block" : "none");   // showKillC
    dom_display("deathCount", rs->show_deaths ? "inline-block" : "none");  // showDeaths
    dom_display("streakCount", rs->show_streak ? "inline-block" : "none"); // showStreak
    dom_display("scoreCount", rs->show_score ? "inline-block" : "none");   // showScore
    dom_display("fpsDisplay", menu_ui()->settings.show_fps ? "" : "none"); // showFPS
    dom_display("scoreZoneCount", "none");
    dom_display("livesCount", "none");
    dom_display("ingressDisplay", rs->show_net_stat ? "block" : "none"); // showNetStat
    dom_display("egressDisplay", rs->show_net_stat ? "block" : "none");
    dom_display("propInstruct", "none");
    dom_display("interactMsg", "none");
    dom_display("interactProg", "none");
    dom_display("teamScores", "none");         // FFA only so far
    dom_display("pingDisplay", rs->show_ping ? "block" : "none");      // showPing
    dom_display("killFeed", !rs->old_scoreboard && rs->show_kills ? "inline-block" : "none"); // showKills
    dom_display("ammoDisplay", "inline-block");

    dom_style("teamName", "opacity", "0");     // FFA: no team pill
    char value[32];
    fmt_opacity(rs->xp_bar_opacity, value, sizeof(value));
    dom_style("igXPBar", "opacity", value);                              // xpBarOpac
    dom_style("igXPBarProg", "background-color", rs->xp_bar_color[0] ? rs->xp_bar_color : XP_BAR_COLOR); // xpBarCol
    if ((el = html_dom_by_id("chatList"))) {
        fmt_opacity(rs->chat_opacity, value, sizeof(value));
        html_dom_set_style(el, "opacity", value);                        // chatOp
        snprintf(value, sizeof(value), "%gpx", rs->chat_height * 100.0);
        html_dom_set_style(el, "max-height", value);                     // chatHeight
        ui_controls_apply_chat_visibility();                             // shwChat: all / on focus / off
    }
    char src[8];
    if ((el = html_dom_by_id("challIcon")) && html_dom_get_attr(el, "src", src, sizeof(src)) < 0) {
        html_dom_set_attr(el, "src", "img/hp_0.png"); // Krunker: challenge mode shows img/skull_0.png instead
    }

    if (visible) {
        hud_ensure_crosshair();
        hud_ensure_hitmarker();
        hud_render_game_info(client);
        client->hud_html.valid = 0;           // re-render every value group this frame
        client->hud_html.next_emit_time = 0.0;
    } else {
        dom_style("aimRecticle", "opacity", "0");
        dom_display("newLeaderDisplay", "none");
        ui->board_shown = 0;
        hud_hide_hit();
        hud_dmg_hide_all();
    }
    if (visible && ui->dead) hud_toggle_game_ui(0);
    // The end screen stays down while the menu is over it (UI.toggleEndScreen ->
    // toggleMenu(true) hid #endUI); it comes back when the menu closes.
    if (ui->ended && !html_ui_is_menu_open()) hud_end_screen_dom();
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
    dom_style("chatHolder", "bottom", "148px");
    dom_style("voiceDisplay", "bottom", "145px");
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

// While spectating (window.Bf5, no live player of ours): hud_spectate.c shows #spectateUI
// and keeps the player holders hidden; this keeps the shared pieces live at 20 Hz:
// the sBoardKey leaderboard (#newLeaderDisplay), the round clock and ping. Kill feed and
// chat rows arrive through their own entry points. Player-only overlays stay off.
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

    const int board = !ui->ended && hud_scoreboard_key(client);
    char board_html[4096];
    board_html[0] = 0;
    if (board) hud_scoreboard_html(client, g_hud_override.scoreboard, board_html, sizeof(board_html));
    if (board != ui->board_shown || (board && hud_hash(board_html) != ui->board_hash)) hud_render_scoreboard(board, board_html);

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

// A fresh page (ready edge): nothing we created or wrote still exists.
static void hud_reset_page(Client *client) {
    HudUi *ui = &g_hud_ui;

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
    client->hud_html.valid = 0;

    const HtmlElem base = html_dom_by_id("uiBase");
    if (base) html_dom_observe_resize(base, on_ui_base_resize, NULL);
}

// Per frame from client_menu_tick(): sample fps, keep the HUD visibility in step with
// the game/menu state, step the hit marker and, at most 20 Hz, rewrite the value
// groups that changed since the last write.
void client_hud_html_tick(Client *client, const float now_f) {
    if (!html_ui_available()) return;

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
    hud->was_ready = ready;
    if (!ready) {
        hud->valid = 0;
        return;
    }

    hud_autoplay(client);
    hud_end_step(client, now * 1000.0);
    hud_feed_step(now * 1000.0);
    // game-overlay text popups (client/src/ui/hud_popups.c): drawn while playing (no kill card / menu)
    hud_popups_tick(now * 1000.0, hud_in_game(client) && !html_ui_is_menu_open());

    // hud.js state.inGame came from the `state` event (client->in_game); the values
    // below are only sampled while the local player is alive.
    const int in_game = client->in_game && client->me;
    if (in_game && !ui->in_game_was) ui->chat_seeded = 0; // new session: welcome lines again
    ui->in_game_was = in_game;
    hud_apply_visibility(client, in_game, in_game && !html_ui_is_menu_open());
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
        if (ui->dmg_last_ms >= 0.0) hud_dmg_hide_all();
        if (ui->nums_last_ms >= 0.0) hud_nums_hide_all();
        hud_speed_step(client, now * 1000.0); // hides it
        hud->valid = 0; // TODO: death screen / end screen (#killCardHolder, #endUI) once the game exposes them
        return;
    }

    hud_demo(client, now);
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
    // aiming cancels an inspect in v3.1.1, so the dot never shows during one; keep that explicit
    next.aim_dot = weapon && weapon_attachment(weapon, DEFAULT_ATTACHMENT_INDEX) && me->aim_val >= 1.0f && !game->config.third_person && !me->inspecting && !ui->ended;
    next.weapon = weapon;
    hud_timer(client, next.timer_text, sizeof(next.timer_text));
    next_extra.spread = (int) lroundf(hud_spread(me));
    next_extra.aim_pct = (int) lroundf(CLAMP(me->aim_val, 0.0f, 1.0f) * 100.0f);
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
    const int loadout_rebuild = force || next_extra.loadout_size != ui->extra.loadout_size ||
                                memcmp(next_extra.loadout, ui->extra.loadout, sizeof(next_extra.loadout)) != 0 ||
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
    }
    if (fps_changed || force) dom_text_int("ingameFPS", hud->fps);
    if (class_changed) hud_render_class(hud_class_icon_index(client, hud->class_index));
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

    // TODO: call hud_ui_add_killfeed() when the game exposes kill events (player_kill()
    // only deactivates the victim today) and hud_ui_hit() when hitscan damage lands
    // (shared/src/player.c hitscan is still a TODO).
}
