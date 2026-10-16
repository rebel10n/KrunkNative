// Krunker Strike's in-game UI (strike_ui.h): the C port of FRVR 1.37.2's IN_GAME_UI (H5) and the
// parts it renders (assets/index-Cz2uYXJQ.js, read as text; minified names kept for reference):
//   H5            .frvr-in-game-ui#InGameUIMobile: [Xfe scorebar, Ude health, efe reload overlay,
//                 jde kill log] while Mt.displayInfoUI, then Yde (the kill card) while Mt.showKillCard
//   Xfe / Yfe     .scorebar-augmented: the score bar (skulls, inner = score / scoreLimit, the lead
//                 marker at scoreLead / scoreLimit, the crown, the star), "• Score: N", "N players",
//                 the round timer (N.getTime) with its circular timer (iB, aM: chill/fine/panic/run-out)
//   Ude / Ode     .frvr-in-game-ui__health-holder: the class face, Sx = 6 bars (each filled
//                 clamp(6 * hp / max - i) * 100 %), "hp / max"
//   efe           .frvr-in-game-ui__ammo-overlay: "Reloading" + the reload bar (visibility)
//   tfe           .frvr-in-game-ui__ammo-overlay--desktop: the reload key's ring and "cur / max", only while
//                 no touch controls are the input (ae().useMobileControls false; strike_touch_input())
//   qde / Xde     .frvr-in-game-ui__msg-holder: "[R] Reload" (game.reload.keyboard) while the magazine is
//                 empty on keyboard input and the mode allows reloads (the bind's key name, not a fixed R)
//   jde / Kde     .frvr-in-game-ui__kill-log-container: Ybe = 3 rows, each dropped 10 s after it came;
//                 killer / victim coloured by AF (friend-colors--you / --enemy / --teammate)
//   Yde           .frvr-in-game-ui__kill-card-container: "Killed By" + the killer's card, or "You Died"
//   Gde / U5      .frvr-in-game-ui__score-board-container: table.mini-score-board (rank, name, the mode's
//                 leaderStats) while the scoreboard key (Tab, hold) is down; not on touch (no key events)
// The death: Mt.showKillCard, the mobile controls hidden, and xe.deathDelay later Cg() (goToPlay):
// a Strike player respawns on their own, no menu in between (strike_hud_death_tick).
// The round end: LEAN_END_SCREEN (WB, the template LEAN_END_SCREEN.html) over the game.
#include <strike_ui.h>
#include <strike_econ.h>
#include <strike_double_xp.h>
#include <strike_hud_fx.h>
#include <render_settings.h>
#include <strike_banner_ad.h>
#include <html_ui.h>
#include <ui_logic.h>
#include <shared.h>
#include <modes.h>
#include <weapons.h>
#include <zombies.h>
#include <i18n.h>
#include <spectate.h>
#include <math.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cJSON.h>

#define KILL_LOG_MAX 3          // Ybe
#define KILL_LOG_MS 10000.0     // a row's setTimeout
#define HEALTH_BARS 6           // Sx
#define KILL_NAME_MAX 48
#define BOARD_ROWS_MAX 64       // U5's rows (FRVR lists every player)
#define BOARD_STAT_KEYS 4       // the most leaderStats columns a mode has (NET_LEADER_STATS_MAX)

typedef struct {
    char killer[KILL_NAME_MAX], victim[KILL_NAME_MAX];
    char weapon[96];            // icon src ("" none)
    int killer_is_you, victim_is_you, killer_team, victim_team, headshot;
    int thrown, wallbang;       // img/thrown_0.png before the weapon, img/wallbang_0.png after it
    int timer;
} KillLogRow;

// One mini-scoreboard row: a live player (stats from mode_leader_stat_value) or a scenario row.
typedef struct {
    const Player *player;       // NULL for a scenario row
    char name[PLAYER_NAME_SIZE];
    int team, is_you, class_index, order;
    int score, kills, deaths;   // scenario rows (a live row takes the mode's stats)
} BoardRow;

static struct {
    Client *client;
    HtmlElem root;              // #InGameUIMobile, 0 while not rendered
    HtmlElem end_root;
    KillLogRow log[KILL_LOG_MAX];
    int log_count;
    int log_dirty;
    // the kill card (Mt.showKillCard / Mt.killCardInfo)
    int kill_card;
    int killed_by;
    char kc_name[KILL_NAME_MAX], kc_weapon[96], kc_pfp[96], kc_color[64];
    char kc_flag[3];            // the killer's Country Flag, upper-case ISO code ("US"); "" none (img/flags/3x2)
    int kc_level, kc_score;
    // the mini scoreboard (Mt.showMiniScoreBoard): this tick's flag and the markup last written
    int board;
    char *last_board;
    // last rendered values (Mithril's diff)
    char last_score[160], last_health[64], last_timer[64], last_reload[32], last_desk[128], last_hint[96];
    int last_card;              // card_key() when the kill card / its Spectate button was drawn
    int end_shown, end_result;  // the round end is up this round (and its result: strike_hud_end)
    int rendered_spec;          // spectate_active() when the HUD was drawn (the panel replaces the health and reload)
    int banner_timer;           // the victory banner (endAnim) until it opens LEAN_END_SCREEN, 0 none
    NetStrikeRoundPacket round; // the commit's XP of this round (NET_EXT_KN_STRIKE_ROUND)
    int round_valid;
    int claim_timer, claim_shown; // the track claim button: 2 s after a level-up; claim_shown once it is up
    int lvl_timer, lvl_now;     // the level number counting up after a level-up
    char *lean_head, *lean_main; // the last header / rewards markup of LEAN_END_SCREEN (NULL: none yet)
    // --render-scenario: a pinned card / health (the reference states hold them while the sandbox player lives)
    int scenario, scn_health, scn_max_health, scn_reward, scn_banner, scn_claim;
    int scn_board, scn_row_count;   // state.strike.scoreboard (held) and state.strike.board (rows)
    int scn_cross;                  // state.strike.crosshair: the Dynamic crosshair shown (cross_tick)
    BoardRow scn_rows[BOARD_ROWS_MAX];
} H;

// ---------------------------------------------------------------- helpers

static const Player *me_player(const Client *client) {
    return client ? client->me : NULL;
}

// N.getTime(seconds): "MM:SS"
static void time_text(const int seconds, char *out, const size_t n) {
    const int s = seconds > 0 ? seconds : 0;
    snprintf(out, n, "%02d:%02d", s / 60, s % 60);
}

// aM(i, e)
static const char *time_modifier(const float i, const float e) {
    if (i >= e * 0.5f) return "chill";
    if (i >= e * 0.25f) return "fine";
    if (i == 0.0f) return "run-out";
    return "panic";
}

// ae().getClassIcon(n): textures/classes/icon_N.png on FRVR's CDN (the page's ./cdn/ copy)
static void class_icon(const int class_index, char *out, const size_t n) {
    snprintf(out, n, "./cdn/textures/classes/icon_%d.png", class_index >= 0 ? class_index : 0);
}

static int score_limit(const Client *client) {
    const GameMode *mode = client->game.mode;
    if (mode && mode->score_limit > 0) return mode->score_limit;
    if (mode && mode->config.score_limit > 0) return mode->config.score_limit;
    return 1500; // nfe (ffa) scoreLimit
}

static int team_mode(const Client *client) {
    return client->game.mode && client->game.mode->config.teams;
}

// ---------------------------------------------------------------- views (FRVR's markup)

// Yfe + Xfe + vF (the scorebar)
static void view_scorebar(const Client *client, StrBuf *sb) {
    const int limit = score_limit(client);
    const Player *me = me_player(client);
    const int score = me ? me->score : 0;
    int lead = 0, total = 0;
    for (size_t i = 0; i < client->game.player_count; i++) {
        const Player *p = client->game.players[i];
        if (!p) continue;
        total++;
        if (p->score > lead) lead = p->score;
    }
    const int teams = team_mode(client);
    const int t0 = teams ? client->game.mode->team_scores[1] : 0, t1 = teams ? client->game.mode->team_scores[2] : 0;
    float r;
    if (teams) r = t0 > t1 ? fminf((float) t0 / limit, 1.0f) * 50.0f : 100.0f - fminf((float) t1 / limit, 1.0f) * 50.0f;
    else r = fminf((float) lead / limit, 1.0f) * 100.0f;
    const int lead_marker = !teams || t0 != t1;

    sb_append(sb, "<div class=\"scorebar-augmented\"><div class=\"scorebar\"><div class=\"scorebar__bar\"><div class=\"scorebar__bar-skulls\"></div>");
    if (teams) {
        sb_appendf(sb, "<div class=\"scorebar__bar-inner scorebar__bar-inner--team0\" style=\"width: %g%%;\"></div>", fminf((float) t0 / limit * 50.0f, 50.0f));
        sb_appendf(sb, "<div class=\"scorebar__bar-inner scorebar__bar-inner--team1\" style=\"width: %g%%;\"></div>", fminf((float) t1 / limit * 50.0f, 50.0f));
    } else {
        sb_appendf(sb, "<div class=\"scorebar__bar-inner\" style=\"width: %g%%;\"></div>", fminf((float) score / limit * 100.0f, 100.0f));
    }
    sb_append(sb, "<div class=\"scorebar__bar-overlay-light\"></div>");
    if (!teams) sb_appendf(sb, "<div class=\"scorebar__score-lead\" style=\"left: %g%%;\"><div class=\"scorebar__score-lead-indicator\"></div></div>", r);
    sb_append(sb, "</div>");
    if (lead_marker) sb_appendf(sb, "<div class=\"scorebar__score-lead\" style=\"left: %g%%;\"><img src=\"./img/frvr-ui/crown-indicator.svg\" class=\"scorebar__score-lead-icon\"></div>", r);
    sb_append(sb, "<img src=\"./img/frvr-ui/star.svg\" class=\"scorebar__star\"></div>");

    int per_team[2] = {0, 0};
    if (teams) {
        for (size_t i = 0; i < client->game.player_count; i++) {
            const Player *p = client->game.players[i];
            if (p && p->team >= 1 && p->team <= 2) per_team[p->team - 1]++;
        }
        sb_append(sb, "<div class=\"scorebar-augmented__container\"><div class=\"scorebar-augmented__contaniner-team\">");
        sb_appendf(sb, "<div class=\"scorebar-augmented__left\"><span>\xE2\x80\xA2  Score: </span><span class=\"scorebar-augmented__gold-text\">%d</span></div>", t0);
        sb_appendf(sb, "<div class=\"scorebar-augmented__right\"><span>%d</span><img src=\"./img/frvr-ui/friends-green.svg\"></div></div>", per_team[0]);
        sb_append(sb, "<div class=\"scorebar-augmented__contaniner-team\">");
        sb_appendf(sb, "<div class=\"scorebar-augmented__left\"><img src=\"./img/frvr-ui/friends-red.svg\"><span>%d</span></div>", per_team[1]);
        sb_appendf(sb, "<div class=\"scorebar-augmented__right\"><span>Score: </span><span class=\"scorebar-augmented__gold-text\">%d</span><span>  \xE2\x80\xA2</span></div></div></div>", t1);
    } else {
        sb_append(sb, "<div class=\"scorebar-augmented__container\">");
        sb_appendf(sb, "<div class=\"scorebar-augmented__left\"><span>\xE2\x80\xA2  Score: </span><span class=\"scorebar-augmented__gold-text\">%d</span></div>", score);
        sb_appendf(sb, "<div class=\"scorebar-augmented__right\"><span>%d</span><span> players</span></div></div>", total);
    }

    // vF(roundTimer, roundTimerMax)
    const int left = client->net_round_seconds >= 0 ? client->net_round_seconds : 0;
    int max = client->game.config.game_time > 0 ? client->game.config.game_time * 60 : 0;
    if (max < left) max = left;
    const float pct = max > 0 ? (float) left / (float) max : 0.0f;
    const char *mod = max > 0 ? time_modifier(pct, 1.0f) : "panic";
    char t[16];
    time_text(left, t, sizeof(t));
    sb_appendf(sb, "<div class=\"scorebar-augmented__timer\"><div class=\"scorebar-augmented__timer-inner\"><div class=\"scorebar-augmented__timer-text scorebar-augmented__timer-text--%s\">%s</div></div>", mod, t);
    if (max > 0) sb_appendf(sb, "<div class=\"circular-timer circular-timer--%s\" style=\"--percent: %g; --size: 20;\"><svg><circle></circle></svg></div></div></div>", mod, pct);
    else sb_appendf(sb, "<div class=\"circular-timer circular-timer--%s\" style=\"--percent: NaN; --size: 20;\"><svg><circle></circle></svg></div></div></div>", mod);
}

// Ude + Ode
static void view_health(const Client *client, StrBuf *sb) {
    const Player *me = me_player(client);
    const float hp = H.scn_max_health ? (float) H.scn_health : me ? fmaxf(me->health, 0.0f) : 0.0f;
    const int max = H.scn_max_health ? H.scn_max_health : me && me->max_health > 0 ? me->max_health : 100;
    char icon[96];
    class_icon(me ? me->class_index : 0, icon, sizeof(icon));
    const float s = hp / (float) max;
    float a = HEALTH_BARS * s;
    const int l = (int) ceilf(s / ((float) max / HEALTH_BARS) * 100.0f);
    sb_append(sb, "<div class=\"frvr-in-game-ui__health-holder\"><div class=\"frvr-in-game-ui__health-container\"><div class=\"frvr-in-game-ui__health-bg\"></div>");
    sb_appendf(sb, "<div class=\"frvr-in-game-ui__health-pfp\"><img src=\"%s\" style=\"image-rendering: pixelated;\"></div>", icon);
    sb_append(sb, "<div class=\"frvr-in-game-ui__health-values-holder\"><div class=\"frvr-in-game-ui__health-bar-holder\">");
    for (int i = 0; i < HEALTH_BARS; i++) {
        const float filled = fmaxf(0.0f, fminf(1.0f, a)) * 100.0f;
        a -= 1.0f;
        sb_append(sb, "<div class=\"frvr-in-game-ui__health-bar\">");
        sb_appendf(sb, "<div class=\"frvr-in-game-ui__health-bar frvr-in-game-ui__health-bar--lose\" style=\"width: %g%%; transition: width 0.3s ease-in %gs;\"></div>", filled, 0.15 * (l - i + 1));
        sb_appendf(sb, "<div class=\"frvr-in-game-ui__health-bar %s\" style=\"width: %g%%; transition: width ease-in;\"></div></div>",
                   s > 0.3f ? "frvr-in-game-ui__health-bar--full" : "frvr-in-game-ui__health-bar--low", filled);
    }
    sb_appendf(sb, "</div><div class=\"frvr-in-game-ui__health-bottom\"><div class=\"frvr-in-game-ui__health-text\">%d / %d</div>", (int) floorf(hp), max);
    sb_append(sb, "<div class=\"frvr-in-game-ui__health-xp-bars\"><div class=\"frvr-in-game-ui__health-xp\"></div>"
                  "<div class=\"frvr-in-game-ui__health-xp frvr-in-game-ui__health-xp--small\"></div>"
                  "<div class=\"frvr-in-game-ui__health-xp frvr-in-game-ui__health-xp--small\"></div></div></div></div></div></div>");
}

// efe: the reload overlay (Mt.reloading, Mt.reloadTimer)
static int reload_ms(const Client *client) {
    const Player *me = me_player(client);
    if (!me || !me->weapon || me->reload_timer <= 0.0f) return 0;
    return (int) lroundf(me->reload_timer * 1000.0f);
}

static void view_reload(const Client *client, StrBuf *sb) {
    const int ms = reload_ms(client);
    sb_appendf(sb, "<div class=\"frvr-in-game-ui__ammo-overlay\" style=\"visibility: %s;\"><span>Reloading</span>", ms > 0 ? "visible" : "hidden");
    sb_appendf(sb, "<div class=\"frvr-in-game-ui__reload-bar-holder\"><div class=\"frvr-in-game-ui__reload-bar\" style=\"width: %d%%; transition: width %gs ease-in;\"></div></div></div>",
               ms > 0 ? 100 : 0, ((ms > 0 ? ms : 0) - 100) / 1000.0);
}

// tfe: the desktop reload overlay shows while the weapon can reload and the keyboard is the input
static int desktop_reload_shown(const Client *client) {
    const Player *me = me_player(client);
    if (!me || !me->active || !me->weapon || strike_touch_input() || spectate_active()) return 0;
    return me->weapon->reload_time > 0.0f && !me->weapon->no_reload; // weapon.reload
}

static void view_reload_desktop(const Client *client, StrBuf *sb) {
    if (!desktop_reload_shown(client)) return;
    char text[32];
    int off = 0, show = 0;
    strike_touch_ammo_text(text, sizeof(text), &off, &show);
    sb_appendf(sb, "<div class=\"frvr-in-game-ui__ammo-overlay frvr-in-game-ui__ammo-overlay--desktop\"><img src=\"./img/controls/reload.svg\">"
                   "<svg><circle style=\"stroke-dashoffset: %d%%; transition: stroke-dashoffset %gs ease-out; visibility: %s;\"></circle></svg>"
                   "<div class=\"frvr-in-game-ui__ammo-label\"><div class=\"frvr-in-game-ui__ammo-label-text\"><span>%s</span></div></div></div>",
               off, reload_ms(client) / 1000.0, show ? "visible" : "hidden", text);
}

// Xde: the keyboard hint shows while the magazine is empty and the mode lets the player reload: no
// ammo limit, a finite mode's reserve is not spent (bots excepted), a melee / magazine-less weapon never
static int reload_hint_shown(const Client *client) {
    Player *me = client ? client->me : NULL;
    const GameMode *mode = client ? client->game.mode : NULL;
    if (!me || !me->active || !me->weapon || !mode || strike_touch_input() || spectate_active()) return 0;
    if (mode->config.no_reloads || mode->config.has_ammo_limit || me->weapon->ammo == 0) return 0;
    const int idx = me->loadout_index;
    if (!me->ammo || idx < 0 || idx >= me->loadout_size || me->ammo[idx] > 0) return 0;
    return zombies_reload_allowed(me); // finiteAmmo: the reserve is not spent (bots excepted)
}

// "[R] Reload" with the Reload bind's key name ("Reload" alone while the bind is unbound)
static void reload_hint_label(char *out, const size_t n) {
    char key[32];
    const int code = ui_controls_bind(UI_CONTROL_RELOAD);
    if (code < 0) {
        snprintf(out, n, "Reload");
        return;
    }
    ui_controls_key_name(code, key, sizeof(key));
    snprintf(out, n, "[%s] Reload", key);
}

static void view_reload_hint(const Client *client, StrBuf *sb) {
    if (!reload_hint_shown(client)) return;
    char label[64];
    reload_hint_label(label, sizeof(label));
    sb_append(sb, "<div class=\"frvr-in-game-ui__msg-holder\"><div class=\"frvr-in-game-ui__message\"><span>");
    sb_append_esc(sb, label);
    sb_append(sb, "</span></div></div>");
}

// Krunker Strike's spectate HUD (no FRVR view: FRVR 1.37.2 never sets Se.isSpectatingOn). Our state
// is spectate.c's (spectate_active, the spectate request); the panel is the dead player's entry point
// (FRVR's strings: menu.btn.spectate.mobile "Spectate" on the kill card, app.spectate.mobile "TAP TO
// SPECTATE"), then the target's name and team, the first / third person label and Prev / Next / Play.
// view_spectate: .frvr-in-game-ui__spectate (replaces the health and reload while spectating)
static void view_spectate(const Client *client, StrBuf *sb) {
    const int uid = spectate_target_uid();
    const Player *target = uid >= 0 ? client_find_player((Client *) client, uid) : NULL;
    sb_append(sb, "<div class=\"frvr-in-game-ui__spectate\"><div class=\"frvr-in-game-ui__spectate-title\">Spectating</div>");
    sb_append(sb, "<div class=\"frvr-in-game-ui__spectate-name\">");
    if (target) sb_append_esc(sb, target->name);
    else sb_append(sb, "Free Camera");
    sb_append(sb, "</div>");
    if (target && team_mode(client) && target->team) sb_appendf(sb, "<div class=\"frvr-in-game-ui__spectate-team\">Team %d</div>", (int) target->team);
    sb_appendf(sb, "<div class=\"frvr-in-game-ui__spectate-person\">%s</div>", spectate_person_label());
    sb_append(sb, "<div class=\"frvr-in-game-ui__spectate-row\">"
                  "<div class=\"frvr-in-game-ui__spectate-button frvr-in-game-ui__spectate-prev\">Prev</div>"
                  "<div class=\"frvr-in-game-ui__spectate-button frvr-in-game-ui__spectate-next\">Next</div>"
                  "<div class=\"frvr-in-game-ui__spectate-button frvr-in-game-ui__spectate-play\">Play</div></div></div>");
}

// AF(): the colour class of a kill-log name
static const char *af_class(const int is_you, const int team, const int my_team) {
    if (is_you) return "friend-colors--you";
    if (team && my_team && team == my_team) return "friend-colors--teammate";
    return "friend-colors--enemy";
}

// jde / Kde
static void view_kill_log(const Client *client, StrBuf *sb) {
    const Player *me = me_player(client);
    const int my_team = me ? me->team : 0;
    sb_append(sb, "<div class=\"frvr-in-game-ui__kill-log-container\">");
    for (int i = 0; i < H.log_count; i++) {
        const KillLogRow *r = &H.log[i];
        sb_append(sb, "<div class=\"frvr-in-game-ui__kill-log-row\">");
        if (r->killer[0]) {
            sb_appendf(sb, "<div class=\"frvr-in-game-ui__kill-log-text %s\">", af_class(r->killer_is_you, r->killer_team, my_team));
            sb_append_esc(sb, r->killer);
            sb_append(sb, "</div>");
        }
        sb_append(sb, "<div class=\"frvr-in-game-ui__kill-log-imgs\">");
        if (r->thrown) sb_append(sb, "<div class=\"frvr-in-game-ui__kill-log-img\"><img src=\"./img/thrown_0.png\"></div>");
        if (r->weapon[0]) sb_appendf(sb, "<div class=\"frvr-in-game-ui__kill-log-img\"><img src=\"%s\"></div>", r->weapon);
        if (r->wallbang) sb_append(sb, "<div class=\"frvr-in-game-ui__kill-log-img\"><img src=\"./img/wallbang_0.png\"></div>");
        if (r->headshot) sb_append(sb, "<div class=\"frvr-in-game-ui__kill-log-img\"><img src=\"./img/headshot_0.png\"></div>");
        sb_append(sb, "</div>");
        sb_appendf(sb, "<div class=\"frvr-in-game-ui__kill-log-text %s\">", af_class(r->victim_is_you, r->victim_team, my_team));
        sb_append_esc(sb, r->victim);
        sb_append(sb, "</div></div>");
    }
    sb_append(sb, "</div>");
}

// Yde
static void view_kill_card(StrBuf *sb) {
    sb_appendf(sb, "<div class=\"frvr-in-game-ui__kill-card-container\"><div class=\"frvr-in-game-ui__kill-card-killedby-holder%s\"><span>%s</span></div>",
               H.killed_by ? "" : " frvr-in-game-ui__kill-card-killedby-holder--died", H.killed_by ? "Killed By" : "You Died");
    if (H.killed_by) {
        sb_appendf(sb, "<div class=\"frvr-in-game-ui__kill-card-card\"><div class=\"frvr-in-game-ui__kill-card-pfp\"><img src=\"%s\"></div>", H.kc_pfp);
        sb_append(sb, "<div class=\"frvr-in-game-ui__kill-card-middle\"><div class=\"frvr-in-game-ui__kill-card-nickname\">");
        sb_append_esc(sb, H.kc_name);
        sb_appendf(sb, "</div><div class=\"frvr-in-game-ui__kill-card-weapon\" style=\"color: %s;\">", H.kc_color);
        sb_append_esc(sb, H.kc_weapon);
        sb_appendf(sb, "</div></div><div class=\"frvr-in-game-ui__kill-card-lvl-flag\"><div class=\"frvr-in-game-ui__kill-card-lvl\">LVL %d</div>", H.kc_level);
        // FRVR's .frvr-in-game-ui__kill-card-flag beside the level (blank when the killer has no flag); ../img is ui/img
        if (H.kc_flag[0]) sb_appendf(sb, "<div class=\"frvr-in-game-ui__kill-card-flag\"><img src=\"../img/flags/3x2/%s.svg\"></div>", H.kc_flag);
        sb_append(sb, "</div>");
        sb_appendf(sb, "<div class=\"frvr-in-game-ui__kill-card-score\">+ %d</div></div>", H.kc_score);
    }
    if (!spectate_requested()) sb_append(sb, "<div class=\"frvr-in-game-ui__kill-card-spectate\">Spectate</div>");
    sb_append(sb, "</div>");
}

// ---------------------------------------------------------------- the mini scoreboard (Gde / U5)

// One stat of a row: the mode's leaderStats key (Player fields through mode_leader_stat_value).
static int row_stat(const BoardRow *r, const char *key) {
    if (r->player) return mode_leader_stat_value(r->player, key);
    if (!strcmp(key, "score")) return r->score;
    if (!strcmp(key, "kills")) return r->kills;
    if (!strcmp(key, "deaths")) return r->deaths;
    return 0;
}

// U5's columns: mode.leaderStats, default ["score"] (FRVR's `s.mode && s.mode.leaderStats`).
static const char *const BOARD_DEFAULT_KEYS[] = {"score", NULL};

static const char *const *board_keys(const Client *client, int *count) {
    const char *const *keys = client && client->game.mode && client->game.mode->config.leader_stats ? client->game.mode->config.leader_stats
                                                                                                    : BOARD_DEFAULT_KEYS;
    int n = 0;
    while (keys[n] && n < BOARD_STAT_KEYS) n++;
    *count = n;
    return keys;
}

static int key_listed(const char *const *keys, const int count, const char *key) {
    for (int i = 0; i < count; i++) {
        if (!strcmp(keys[i], key)) return 1;
    }
    return 0;
}

// Se.leaderboardData normalised (normalizeData): every player, highest score first (the server's
// board order; ties keep the game's player order).
static int row_compare(const void *a, const void *b) {
    const BoardRow *x = a, *y = b;
    if (x->score != y->score) return x->score > y->score ? -1 : 1;
    return x->order - y->order;
}

static int board_rows(const Client *client, BoardRow *out, const int max) {
    int n = 0;
    if (H.scn_row_count > 0) {
        for (int i = 0; i < H.scn_row_count && n < max; i++) {
            out[n] = H.scn_rows[i];
            out[n].order = n;
            n++;
        }
    } else if (client && client->game.players) {
        for (size_t i = 0; i < client->game.player_count && n < max; i++) {
            const Player *p = client->game.players[i];
            if (!p) continue;
            BoardRow *r = &out[n];
            memset(r, 0, sizeof(*r));
            r->player = p;
            snprintf(r->name, sizeof(r->name), "%s", p->name);
            r->team = p->team;
            r->is_you = p == client->me;
            r->class_index = p->class_index;
            r->score = p->score;
            r->order = n++;
        }
    }
    qsort(out, (size_t) n, sizeof(out[0]), row_compare);
    return n;
}

// getTranslatedStatName: "generic.<key>" (the key itself when no table has it)
static void board_label(const char *key, char *out, const size_t n) {
    char id[64];
    snprintf(id, sizeof(id), "generic.%s", key);
    snprintf(out, n, "%s", i18n_tr(id, key));
}

// normalizeData's team names: teamOptions, else FRVR's default team names
static const char *board_team_name(const Client *client, const int team) {
    const GameMode *mode = client->game.mode;
    if (team >= 1 && team <= 2) {
        if (mode && mode->config.team_options[team - 1]) return mode->config.team_options[team - 1];
        return team == 2 ? "Bravo" : "Alpha";
    }
    return "";
}

// Gde: .frvr-in-game-ui__score-board-container > .frvr-in-game-ui__score-board > table.mini-score-board
// (U5.view): the header row, then per team (in team modes) a team header and its players, or one
// group of the players. Rank = the place within the group.
static void view_board(const Client *client, StrBuf *sb) {
    BoardRow rows[BOARD_ROWS_MAX];
    const int n = board_rows(client, rows, BOARD_ROWS_MAX);
    int columns;
    const char *const *keys = board_keys(client, &columns);
    const int teams = team_mode(client);
    sb_append(sb, "<div class=\"frvr-in-game-ui__score-board-container\"><div class=\"frvr-in-game-ui__score-board\"><table class=\"mini-score-board\">");
    sb_append(sb, "<tr class=\"header-row\"><td class=\"top-left\">#</td><td></td><td></td>");
    for (int k = 0; k < columns; k++) {
        char label[64];
        board_label(keys[k], label, sizeof(label));
        sb_appendf(sb, "<td class=\"mini-score-board__%s\">", keys[k]);
        sb_append_esc(sb, label);
        sb_append(sb, "</td>");
    }
    sb_append(sb, "</tr>");
    int done[BOARD_ROWS_MAX] = {0};
    for (int i = 0; i < n; i++) {
        if (done[i]) continue;
        const int team = rows[i].team;
        if (teams) { // getTeamHeader: the team's score, then the sums of kills, deaths and assists
            const char *sums_of[3] = {"kills", "deaths", "assists"};
            int sums[3] = {0, 0, 0};
            for (int j = 0; j < n; j++) {
                if (rows[j].team != team) continue;
                for (int s = 0; s < 3; s++) sums[s] += key_listed(keys, columns, sums_of[s]) ? row_stat(&rows[j], sums_of[s]) : 0;
            }
            const int team_score = client->game.mode && (team == 1 || team == 2) ? client->game.mode->team_scores[team] : 0;
            sb_append(sb, "<tr class=\"mini-score-board__team-header\"><td colspan=\"3\">");
            sb_append_esc(sb, board_team_name(client, team));
            sb_appendf(sb, "</td><td class=\"mini-score-board__team-stat\">%d</td><td class=\"mini-score-board__team-stat\">%d</td>"
                           "<td class=\"mini-score-board__team-stat\">%d</td><td class=\"mini-score-board__team-stat\">%d</td></tr>",
                       team_score, sums[0], sums[1], sums[2]);
        }
        int pos = 0;
        for (int j = i; j < n; j++) {
            if (done[j] || rows[j].team != team) continue;
            done[j] = 1;
            char icon[96];
            class_icon(rows[j].class_index, icon, sizeof(icon));
            sb_appendf(sb, "<tr class=\"data-row%s\">", rows[j].is_you ? " mini-score-board__thats-you-row" : "");
            sb_appendf(sb, "<td class=\"mini-score-board__pos\">%d</td><td class=\"mini-score-board__avatar\"><img src=\"%s\"></td>"
                           "<td class=\"mini-score-board__name\">", ++pos, icon);
            sb_append_esc(sb, rows[j].name);
            sb_append(sb, "</td>");
            for (int k = 0; k < columns; k++) sb_appendf(sb, "<td class=\"mini-score-board__%s\">%d</td>", keys[k], row_stat(&rows[j], keys[k]));
            sb_append(sb, "</tr>");
        }
    }
    sb_append(sb, "</table></div></div>");
}

// what the kill card shows: none, or the card with its Spectate button (until it is requested)
static int card_key(void) { return H.kill_card ? 1 + spectate_requested() : 0; }

// ---------------------------------------------------------------- the in-game feedback (strike_hud_fx.h)

// FRVR draws the damage numbers and the crosshair on the game canvas, under its DOM HUD. Here they are a
// layer (#knStrikeFx, the first child of #InGameUIMobile): they live and die with the HUD (in game, the menu
// closed, not the end screen) and sit under the HUD's text. The rules are strike_hud_fx.c's.
static struct {
    StrikeDamageSet dmg;               // the live numbers
    double clock_ms;                   // hud_ui_now_ms at the last step; clock_valid: it has one
    int clock_valid;
    HtmlElem layer;                    // #knStrikeFx, 0 while the HUD is not rendered
    HtmlElem dmg_el[STRIKE_DMG_MAX];   // a number's element, made on its first use
    char dmg_text[STRIKE_DMG_MAX][24]; // the text an element shows (rewritten only on a change)
    HtmlElem cross;                    // #knStrikeCross: the page centre, the Dynamic crosshair's origin
    HtmlElem cross_bar[4];             // top, right, bottom, left
    HtmlElem cross_dot;
    float cross_scale;                 // the spread radius the lines are placed for (-1: not placed)
} FX = {.cross_scale = -1.0f};

static void fx_forget(void) {
    strike_damage_reset(&FX.dmg);
    FX.clock_valid = 0;
    FX.layer = FX.cross = FX.cross_dot = 0;
    memset(FX.dmg_el, 0, sizeof(FX.dmg_el));
    memset(FX.dmg_text, 0, sizeof(FX.dmg_text));
    memset(FX.cross_bar, 0, sizeof(FX.cross_bar));
    FX.cross_scale = -1.0f;
}

// The markup: the layer, then the crosshair's origin with its four lines and its dot (sized and coloured by
// fx_mount / cross_layout / cross_style)
static void view_fx(StrBuf *sb) {
    sb_append(sb, "<div id=\"knStrikeFx\" class=\"kn-strike-fx\" style=\"position:absolute;left:0;top:0;width:100%;height:100%;"
                  "pointer-events:none;overflow:hidden\">"
                  "<div id=\"knStrikeCross\" class=\"kn-strike-cross\" style=\"position:absolute;left:50%;top:50%;width:0;height:0;"
                  "display:none\">"
                  "<div class=\"kn-cross-bar\" style=\"position:absolute;box-sizing:border-box\"></div>"
                  "<div class=\"kn-cross-bar\" style=\"position:absolute;box-sizing:border-box\"></div>"
                  "<div class=\"kn-cross-bar\" style=\"position:absolute;box-sizing:border-box\"></div>"
                  "<div class=\"kn-cross-bar\" style=\"position:absolute;box-sizing:border-box\"></div>"
                  "<div class=\"kn-cross-dot\" style=\"position:absolute\"></div>"
                  "</div></div>");
}

static void cross_style(void) {
    const ClientRenderSettings *rs = client_render_settings();
    char border[96];
    snprintf(border, sizeof(border), "%gpx solid %s", STRIKE_CROSS_OUTLINE, rs->crosshair_shadow);
    for (int i = 0; i < 4; i++) {
        if (!FX.cross_bar[i]) continue;
        html_dom_set_style(FX.cross_bar[i], "background", rs->crosshair_color);
        html_dom_set_style(FX.cross_bar[i], "border", border);
    }
    if (FX.cross_dot) html_dom_set_style(FX.cross_dot, "background", rs->crosshair_color);
}

static void place(const HtmlElem el, const float x, const float y, const float w, const float h) {
    if (!el) return;
    char v[32];
    snprintf(v, sizeof(v), "%.2fpx", x);
    html_dom_set_style(el, "left", v);
    snprintf(v, sizeof(v), "%.2fpx", y);
    html_dom_set_style(el, "top", v);
    snprintf(v, sizeof(v), "%.2fpx", w);
    html_dom_set_style(el, "width", v);
    snprintf(v, sizeof(v), "%.2fpx", h);
    html_dom_set_style(el, "height", v);
}

// The lines for a spread radius (strike_crosshair_span): each is a 6 x 23 px rectangle whose inner end is
// `near` and outer end `far` from the centre; the top one is above it, and so on
static void cross_layout(const float scale) {
    float near_d = 0.0f, far_d = 0.0f;
    strike_crosshair_span(scale, &near_d, &far_d);
    const float t = STRIKE_CROSS_BAR_THICK, l = STRIKE_CROSS_BAR_LEN;
    place(FX.cross_bar[0], -t / 2.0f, -far_d, t, l); // top: y from -far to -near
    place(FX.cross_bar[1], near_d, -t / 2.0f, l, t); // right: x from near to far
    place(FX.cross_bar[2], -t / 2.0f, near_d, t, l); // bottom
    place(FX.cross_bar[3], -far_d, -t / 2.0f, l, t); // left
    place(FX.cross_dot, -STRIKE_CROSS_DOT / 2.0f, -STRIKE_CROSS_DOT / 2.0f, STRIKE_CROSS_DOT, STRIKE_CROSS_DOT);
    FX.cross_scale = scale;
}

// after the markup is in: the elements the layer holds (their numbers are made on demand)
static void fx_mount(void) {
    FX.layer = html_dom_by_id("knStrikeFx");
    FX.cross = html_dom_by_id("knStrikeCross");
    HtmlElem bars[4];
    const int n = FX.layer ? html_dom_query_all(FX.layer, ".kn-cross-bar", bars, 4) : 0;
    for (int i = 0; i < 4; i++) FX.cross_bar[i] = i < n ? bars[i] : 0;
    FX.cross_dot = FX.layer ? html_dom_query(FX.layer, ".kn-cross-dot") : 0;
    memset(FX.dmg_el, 0, sizeof(FX.dmg_el));
    memset(FX.dmg_text, 0, sizeof(FX.dmg_text));
    FX.cross_scale = -1.0f;
    cross_style();
}

// A number's element (.kn-strike-dmg): a block whose bottom centre sits on the number's page point, as
// FRVR's fillText(val, -width / 2, 0) puts the text's baseline there; in GameFont like the canvas. It exists
// only while its number is on screen, so the page holds exactly the numbers a player sees.
static HtmlElem fx_number(const int i) {
    if (FX.dmg_el[i]) return FX.dmg_el[i];
    if (!FX.layer) return 0;
    const HtmlElem el = html_dom_create("div", NULL);
    if (!el) return 0;
    html_dom_set_attr(el, "class", "kn-strike-dmg");
    html_dom_set_style(el, "position", "absolute");
    html_dom_set_style(el, "white-space", "nowrap");
    html_dom_set_style(el, "line-height", "1");
    html_dom_set_style(el, "pointer-events", "none");
    html_dom_set_style(el, "font-family", "GameFont");
    html_dom_set_style(el, "transform", "translate(-50%, -80%)");
    html_dom_append(FX.layer, el);
    FX.dmg_el[i] = el;
    FX.dmg_text[i][0] = 0;
    return el;
}

static void fx_number_drop(const int i) {
    if (FX.dmg_el[i]) html_dom_remove(FX.dmg_el[i]);
    FX.dmg_el[i] = 0;
    FX.dmg_text[i][0] = 0;
}

// the eight-way ring of the stroke (FRVR's strokeText with lineWidth 8: about 3 px around the glyphs)
static void ring_shadow(const char *colour, char *out, const size_t n) {
    snprintf(out, n, "3px 0 %s, -3px 0 %s, 0 3px %s, 0 -3px %s, 2px 2px %s, -2px -2px %s, 2px -2px %s, -2px 2px %s", colour, colour, colour,
             colour, colour, colour, colour, colour);
}

// FRVR's draw of newDmgNums / dmgNums: a number in the camera's frustum is drawn at its projected point
// nudged by offsetX / offsetY (left and up), at dmgScale px (1.1 times for a crit), its fill and stroke
// the mode's colours, its alpha strike_damage_alpha; one off screen or dead is not drawn at all
static void fx_numbers_tick(Client *client) {
    const ClientRenderSettings *rs = client_render_settings();
    for (int i = 0; i < STRIKE_DMG_MAX; i++) {
        const StrikeDamageNum *n = &FX.dmg.nums[i];
        double sx = 0.0, sy = 0.0;
        if (!n->used || !hud_ui_project(client, n->x, n->y, n->z, &sx, &sy)) {
            fx_number_drop(i);
            continue;
        }
        const HtmlElem el = fx_number(i);
        if (!el) continue;
        char v[64], fill[24], stroke[24], shadow[320], text[24];
        if (n->mode == STRIKE_DMG_LEGACY) { // the Legacy draw (FRVR's Wwe): critColor, else dmgColor (on #DE292F, off white), #2e2e2e stroke
            snprintf(fill, sizeof(fill), "%s", n->crit ? rs->crit_color : rs->dmg_color ? "#DE292F" : "#ffffff");
            snprintf(stroke, sizeof(stroke), "%s", "#2e2e2e");
        } else {
            strike_damage_color(n, 0, fill, sizeof(fill));
            strike_damage_color(n, 1, stroke, sizeof(stroke));
        }
        ring_shadow(stroke, shadow, sizeof(shadow));
        snprintf(v, sizeof(v), "%.3f%%", (sx - n->offset_x) * 100.0);
        html_dom_set_style(el, "left", v);
        snprintf(v, sizeof(v), "%.3f%%", (sy - n->offset_y) * 100.0);
        html_dom_set_style(el, "top", v);
        snprintf(v, sizeof(v), "%.2fpx", strike_damage_font_px(n, rs->dmg_scale));
        html_dom_set_style(el, "font-size", v);
        snprintf(v, sizeof(v), "%.3f", strike_damage_alpha(n));
        html_dom_set_style(el, "opacity", v);
        html_dom_set_style(el, "color", fill);
        html_dom_set_style(el, "text-shadow", shadow);
        strike_damage_text(n->value, text, sizeof(text));
        if (strcmp(FX.dmg_text[i], text)) {
            html_dom_set_text(el, text);
            snprintf(FX.dmg_text[i], sizeof(FX.dmg_text[i]), "%s", text);
        }
    }
}

// The Dynamic crosshair (crosshairSho 1, FRVR's default; type 3 draws the same lines): FRVR draws it while
// in play with the mini scoreboard down and not spectating (a dead player's kill card replaces the view),
// its opacity and spread from the player (strike_crosshair_opacity / _scale). Strike's settings have no
// crosshair rows, so the type is the default unless the store says otherwise; a scenario shows it only
// when its state asks (state.strike.crosshair), FRVR's DOM captures have none (the canvas is not in them).
static void cross_tick(Client *client) {
    if (!FX.cross) return;
    const ClientRenderSettings *rs = client_render_settings();
    const Player *me = me_player(client);
    const int type_ok = rs->crosshair_type == 1 || rs->crosshair_type == 3;
    const int shown = H.scenario ? H.scn_cross : type_ok && !H.board && !spectate_active() && !H.kill_card && me && me->active;
    const float scale = strike_crosshair_scale(me ? me->spread : 0.0f, strike_ui_page_height());
    if (fabsf(scale - FX.cross_scale) > 1e-3f) cross_layout(scale);
    const float opacity = !shown ? 0.0f
                          : me   ? strike_crosshair_opacity(me->aim_val, me->inspecting, me->weapon ? me->weapon->no_cross : 0, me->reload_timer)
                                 : 1.0f;
    char v[16];
    snprintf(v, sizeof(v), "%.3f", opacity);
    html_dom_set_style(FX.cross, "opacity", v);
    html_dom_set_display(FX.cross, shown && opacity > 0.0f ? "block" : "none");
}

static void fx_tick(Client *client) {
    if (!FX.layer) return;
    const double now = hud_ui_now_ms(client);
    const double dt = FX.clock_valid ? now - FX.clock_ms : 0.0;
    FX.clock_ms = now;
    FX.clock_valid = 1;
    const Camera *cam = &client->camera;
    strike_damage_step(&FX.dmg, dt, cam->position.x, cam->position.y, cam->position.z, cam->zoom, game_constants.player_scale,
                       game_constants.player_height);
    fx_numbers_tick(client);
    cross_tick(client);
}

// ---------------------------------------------------------------- rendering

static HtmlElem holder(void) { return html_dom_by_id("frvrMenuMobileHolder"); }

// rwe(): the in-game UI sits after the screens, before the controls and the notifications holder
static HtmlElem game_anchor(const HtmlElem h) {
    const HtmlElem a = html_dom_query(h, ".joystick-zones, .control-buttons, .frvr-notifications-holder");
    return a;
}

static HtmlElem part(const char *sel) { return H.root ? html_dom_query(H.root, sel) : 0; }

static void replace_part(const char *sel, const char *html) {
    const HtmlElem old = part(sel);
    if (!old) return;
    html_dom_insert_html(old, HTML_INSERT_BEFORE_BEGIN, html);
    html_dom_remove(old);
}

// The board comes and goes with H.board (the scoreboard key); its markup is rewritten only when it
// changed, and it sits before the kill log (H5's child order: scorebar, health, reload, board, log).
static void update_board(const Client *client) {
    const HtmlElem old = part(".frvr-in-game-ui__score-board-container");
    if (!H.board) {
        if (old) html_dom_remove(old);
        free(H.last_board);
        H.last_board = NULL;
        return;
    }
    StrBuf sb = {0};
    view_board(client, &sb);
    char *html = sb_finish(&sb);
    if (old && H.last_board && !strcmp(html, H.last_board)) {
        free(html);
        return;
    }
    free(H.last_board);
    H.last_board = html; // kept for the next tick's comparison
    if (old) {
        replace_part(".frvr-in-game-ui__score-board-container", html);
        return;
    }
    const HtmlElem anchor = part(".frvr-in-game-ui__kill-log-container");
    if (anchor) html_dom_insert_html(anchor, HTML_INSERT_BEFORE_BEGIN, html);
    else html_dom_insert_html(H.root, HTML_INSERT_BEFORE_END, html);
}

static void render(Client *client) {
    const HtmlElem h = holder();
    if (!h) return;
    const int spec = spectate_active();
    StrBuf sb = {0};
    sb_append(&sb, "<div id=\"InGameUIMobile\" class=\"frvr-in-game-ui\">");
    view_fx(&sb);
    view_scorebar(client, &sb);
    if (spec) {
        view_spectate(client, &sb);
    } else {
        view_health(client, &sb);
        view_reload(client, &sb);
        view_reload_desktop(client, &sb);
        view_reload_hint(client, &sb);
    }
    view_kill_log(client, &sb);
    if (H.kill_card && !spec) view_kill_card(&sb);
    sb_append(&sb, "</div>");
    char *html = sb_finish(&sb);
    const HtmlElem anchor = game_anchor(h);
    if (anchor) html_dom_insert_html(anchor, HTML_INSERT_BEFORE_BEGIN, html);
    else html_dom_insert_html(h, HTML_INSERT_BEFORE_END, html);
    free(html);
    H.root = html_dom_by_id("InGameUIMobile");
    fx_mount();
    H.log_dirty = 0;
    H.last_card = card_key();
    H.rendered_spec = spec;
}

static void unrender(void) {
    if (H.root) html_dom_remove(H.root);
    H.root = 0;
    H.rendered_spec = 0;
    fx_forget(); // the numbers and the crosshair go with the HUD (a fresh render starts them again)
    H.last_score[0] = H.last_health[0] = H.last_timer[0] = H.last_reload[0] = H.last_desk[0] = H.last_hint[0] = 0;
    free(H.last_board);
    H.last_board = NULL;
}

// Mithril redraw: only the parts whose inputs changed are rebuilt (keeps CSS transitions /
// animations of the others running)
static void update(Client *client) {
    const Player *me = me_player(client);
    char key[160];
    StrBuf sb = {0};
    char *html;

    int lead = 0, total = 0;
    for (size_t i = 0; i < client->game.player_count; i++) {
        const Player *p = client->game.players[i];
        if (!p) continue;
        total++;
        if (p->score > lead) lead = p->score;
    }
    snprintf(key, sizeof(key), "%d/%d/%d/%d/%d/%d/%d/%d", me ? me->score : 0, lead, total, score_limit(client), client->net_round_seconds,
             team_mode(client) ? client->game.mode->team_scores[1] : -1, team_mode(client) ? client->game.mode->team_scores[2] : -1,
             client->game.config.game_time);
    if (strcmp(key, H.last_score)) {
        snprintf(H.last_score, sizeof(H.last_score), "%s", key);
        view_scorebar(client, &sb);
        html = sb_finish(&sb);
        replace_part(".scorebar-augmented", html);
        free(html);
    }
    if (H.rendered_spec) {
        const int uid = spectate_target_uid();
        const Player *target = uid >= 0 ? client_find_player(client, uid) : NULL;
        snprintf(key, sizeof(key), "%d/%s/%s", uid, target ? target->name : "", spectate_person_label());
        if (strcmp(key, H.last_health)) {
            snprintf(H.last_health, sizeof(H.last_health), "%s", key);
            view_spectate(client, &sb);
            html = sb_finish(&sb);
            replace_part(".frvr-in-game-ui__spectate", html);
            free(html);
        }
    } else {
        snprintf(key, sizeof(key), "%d/%d/%d/%d", me ? (int) floorf(me->health * 100.0f) : 0, me ? me->max_health : 0, me ? me->class_index : 0,
                 H.scn_health + H.scn_max_health);
        if (strcmp(key, H.last_health)) {
            snprintf(H.last_health, sizeof(H.last_health), "%s", key);
            view_health(client, &sb);
            html = sb_finish(&sb);
            replace_part(".frvr-in-game-ui__health-holder", html);
            free(html);
        }
        snprintf(key, sizeof(key), "%d", reload_ms(client) > 0);
        if (strcmp(key, H.last_reload)) {
            snprintf(H.last_reload, sizeof(H.last_reload), "%s", key);
            view_reload(client, &sb);
            html = sb_finish(&sb);
            replace_part(".frvr-in-game-ui__ammo-overlay", html);
            free(html);
        }
    }
    // the desktop overlay and the keyboard hint come and go with their gates: rebuilt when their state changes
    char desk[128] = "";
    if (desktop_reload_shown(client)) {
        char text[32];
        int off = 0, show = 0;
        strike_touch_ammo_text(text, sizeof(text), &off, &show);
        snprintf(desk, sizeof(desk), "%s|%d|%d|%d", text, off, show, reload_ms(client) > 0);
    }
    if (strcmp(desk, H.last_desk)) {
        snprintf(H.last_desk, sizeof(H.last_desk), "%s", desk);
        const HtmlElem old = part(".frvr-in-game-ui__ammo-overlay--desktop");
        if (old) html_dom_remove(old);
        if (desk[0]) {
            view_reload_desktop(client, &sb);
            html = sb_finish(&sb);
            html_dom_insert_html(H.root, HTML_INSERT_BEFORE_END, html);
            free(html);
        }
    }
    char hint[96] = "";
    if (reload_hint_shown(client)) reload_hint_label(hint, sizeof(hint));
    if (strcmp(hint, H.last_hint)) {
        snprintf(H.last_hint, sizeof(H.last_hint), "%s", hint);
        const HtmlElem old = part(".frvr-in-game-ui__msg-holder");
        if (old) html_dom_remove(old);
        if (hint[0]) {
            view_reload_hint(client, &sb);
            html = sb_finish(&sb);
            html_dom_insert_html(H.root, HTML_INSERT_BEFORE_END, html);
            free(html);
        }
    }
    if (H.log_dirty) {
        H.log_dirty = 0;
        view_kill_log(client, &sb);
        html = sb_finish(&sb);
        replace_part(".frvr-in-game-ui__kill-log-container", html);
        free(html);
    }
    if (card_key() != H.last_card) {
        H.last_card = card_key();
        const HtmlElem old = part(".frvr-in-game-ui__kill-card-container");
        if (old) html_dom_remove(old);
        if (H.kill_card) {
            view_kill_card(&sb);
            html = sb_finish(&sb);
            html_dom_insert_html(H.root, HTML_INSERT_BEFORE_END, html);
            free(html);
        }
    }
    update_board(client);
}

// ---------------------------------------------------------------- the round end (LEAN_END_SCREEN)

// the progress bar (the bundle's progress-bar hd at size 252, text horizontal, --end-screen): the
// bar's segments are 252 x 175/193, 4/193 and 10/193 wide
#define LEAN_BAR_WIDTH 252.0
#define LEAN_BAR_DX (175.0 / 193.0)
#define LEAN_BAR_MIDDLE (4.0 / 193.0)
#define LEAN_BAR_RIGHT (10.0 / 193.0)
#define LEAN_CLAIM_MS 2000.0    // WB.computeNextReward: showClaimNextReward 2 s after the screen is built
#define LEAN_LEVEL_MS 1500.0    // progress-bar hd: the level number counts up again after a level-up

// tB's copies: the stroke copy of each text, then the copy with its class (classes: gold / silver / bronze / place);
// size "--small" (the lean screen, stroke 2) or "--large" (the banner, stroke 4)
static void victory_copy(StrBuf *sb, const int stroke, const char *cls, const char *text, const char *size) {
    sb_append(sb, "<div class=\"frvr-in-game-ui__victory-text ");
    if (stroke) sb_append(sb, "frvr-in-game-ui__victory-text--stroke ");
    else if (cls) sb_appendf(sb, "%s ", cls);
    sb_appendf(sb, "frvr-in-game-ui__victory-text%s\"", size);
    if (stroke) sb_appendf(sb, " style=\"--stroke-size: %d;\"", strcmp(size, "--small") ? 4 : 2);
    sb_appendf(sb, ">%s</div>", text);
}

// _Ae(isFFA, rank, victoryState) through tB: FFA "#1 VICTORY" (gold), "2nd PLACE" (silver), "3rd
// PLACE" (bronze), "Nth PLACE" (place); a team mode VICTORY / DEFEAT / DRAW, plain
static void lean_words(const int ffa, const int rank, const int result, char *first, const size_t fn, char *second, const size_t sn,
                       const char **cls) {
    first[0] = 0;
    *cls = NULL;
    if (ffa && rank == 1) {
        snprintf(first, fn, "#1");
        *cls = "frvr-in-game-ui__victory-text--gold";
        snprintf(second, sn, " VICTORY");
    } else if (ffa) {
        snprintf(first, fn, "%d%s", rank, rank == 2 ? "nd" : rank == 3 ? "rd" : "th");
        *cls = rank == 2 ? "frvr-in-game-ui__victory-text--silver" : rank == 3 ? "frvr-in-game-ui__victory-text--bronze" : "frvr-in-game-ui__victory-text--place";
        snprintf(second, sn, " PLACE");
    } else {
        snprintf(second, sn, "%s", result > 0 ? "VICTORY" : result == 0 ? "DEFEAT" : "DRAW");
    }
}

static void lean_title(StrBuf *sb, const int ffa, const int rank, const int result) {
    char first[16], second[24];
    const char *cls;
    lean_words(ffa, rank, result, first, sizeof(first), second, sizeof(second), &cls);
    // FRVR's img gets no victory state class here (_Ae returns no victoryState)
    sb_append(sb, "<div class=\"lean-end-screen__victory-defeat\"><img src=\"./img/victory-screen-bg.png\" class=\"lean-end-screen__victory-defeat--small \">");
    sb_append(sb, "<div class=\"frvr-in-game-ui__victory-text-holder\">");
    if (first[0]) victory_copy(sb, 1, NULL, first, "--small");
    victory_copy(sb, 1, NULL, second, "--small");
    sb_append(sb, "</div><div class=\"frvr-in-game-ui__victory-text-holder\">");
    if (first[0]) victory_copy(sb, 0, cls, first, "--small");
    victory_copy(sb, 0, NULL, second, "--small");
    sb_append(sb, "</div></div>");
}

// the next level's reward image: the first reward of the track slot (free track: the first one), the
// currency images of pa(reward); items and features have no image without FRVR's item definitions
static const char *next_reward_image(const StrikeTrackSlot *slot, char *buf, const size_t n) {
    if (!slot || slot->reward_count <= 0) return NULL;
    const StrikeReward *r = &slot->rewards[0];
    if (strcmp(r->type, "currency")) return NULL;
    if (!strcmp(r->currency, "kr")) return "./img/currencies/krunkies.png";
    if (!strcmp(r->currency, "xp")) return "./img/frvr-ui/xp-symbol.svg";
    if (!strcmp(r->currency, "spin_ticket")) {
        if (r->amount >= 1 && r->amount <= 3) snprintf(buf, n, "./img/frvr-ui/spin-ticket-%d.png", r->amount);
        else snprintf(buf, n, "./img/frvr-ui/spin-ticket-many.png");
        return buf;
    }
    return NULL;
}

// the round as the lean screen reads it: the account's XP after the round and the XP the round gave
// (NET_EXT_KN_STRIKE_ROUND; the account's own XP until it arrives), the levels before and after, and
// the track's slots. WB.computeLevelProgression / computeNextReward.
typedef struct {
    int64_t xp, gained;
    StrikeLevel now, prev;      // the level after the round / before it (xp - gained)
    int lvl_up, in_cap;
    int claimable;              // a level-up whose track slot can be claimed now (not a choice slot)
    const StrikeTrackSlot *cur; // the level reached: its slot
    const StrikeTrackSlot *next; // the group card's reward: the reached level's, or the next level's
} LeanState;

static void lean_state(LeanState *st) {
    const StrikeAccount *acc = strike_econ_account();
    st->xp = H.round_valid ? H.round.xp : acc->xp;
    st->gained = H.round_valid ? H.round.xp_gained : 0;
    st->now = strike_level_for_xp(st->xp);
    st->prev = strike_level_for_xp(st->xp - st->gained);
    st->lvl_up = st->now.level > st->prev.level;
    st->in_cap = acc->max_level > 0 && st->now.level >= acc->max_level;
    st->cur = strike_track_level(st->now.level);
    st->claimable = st->lvl_up && st->cur && !st->cur->player_choice && !strike_econ_track_claimed(st->cur->slot_id);
    st->next = strike_track_level(st->claimable ? st->now.level : st->now.level + 1);
}

// showClaimNextReward: the claim button is up after LEAN_CLAIM_MS (or at once in a scenario)
static int claim_visible(const LeanState *st) { return st->claimable && (H.claim_shown || H.scn_claim); }

// the lean screen's timers and its last markup (a new screen rebuilds them)
static void lean_timers_cancel(void) {
    if (H.claim_timer) html_timer_cancel(H.claim_timer);
    if (H.lvl_timer) html_timer_cancel(H.lvl_timer);
    H.claim_timer = H.lvl_timer = 0;
    H.claim_shown = 0;
}

static void lean_forget(void) {
    free(H.lean_head);
    free(H.lean_main);
    H.lean_head = H.lean_main = NULL;
}

// the group card (WB.buildRewards: _F with the group's images, the text "Next level reward" or the Claim button)
static void lean_group_html(StrBuf *sb, const LeanState *st, const int claim) {
    char image_buf[96];
    const char *image = next_reward_image(st->next, image_buf, sizeof(image_buf));
    const int group = st->next && st->next->reward_count > 0; // the group's one reward (its image when we have it)
    sb_append(sb, "<div class=\"lean-end-screen__reward\"><div class=\"lean-end-screen__reward-top\">");
    if (image) sb_appendf(sb, "<img src=\"%s\" class=\"lean-end-screen__reward-image \" style=\"image-rendering: unset;\">", image);
    sb_append(sb, "</div><div class=\"lean-end-screen__reward-bottom\">");
    if (claim) // knStrikeClaimLevel: strike_econ_claim_level(slot, -1), the track's claim (POST progression)
        sb_appendf(sb, "<div class=\"frvr-button frvr-button--size-xs frvr-button--width-90\" onclick=\"knStrikeClaimLevel('%s',-1)\">"
                       "<span class=\"frvr-button-text\">Claim</span></div>", st->cur->slot_id);
    else
        sb_appendf(sb, "<div class=\"lean-end-screen__reward-text%s\">Next level reward</div>", group ? " lean-end-screen__reward-text--small-text" : " ");
    sb_append(sb, "</div></div>");
}

// the lean screen's progress bar and rewards (WB.view): the account's level and XP, the XP gained
// (the XP card, the bar's previous fill), the KR the server sent this player (player.reward), the group card
static void lean_main(StrBuf *sb, const LeanState *st, const int reward) {
    const double prog = st->now.xp_to_next > 0 ? (double) st->now.xp_in_level / (double) st->now.xp_to_next : 0.0;
    const double prev = st->prev.xp_to_next > 0 ? (double) st->prev.xp_in_level / (double) st->prev.xp_to_next : 0.0;
    const double bar = LEAN_BAR_WIDTH * LEAN_BAR_DX;
    const double fill = bar * prog;
    const int lvl = st->lvl_up ? st->now.level - 1 : st->now.level; // hd's leftValue: counts up after a level-up
    sb_append(sb, "<div class=\"progress-bar__progress-bar-holder progress-bar--animate progress-bar--text-horizontal\" style=\"--progress-width: 252;\">");
    sb_append(sb, "<div class=\"progress-bar__horizontal-holder\"><div class=\"progress-bar__left-text\">");
    sb_appendf(sb, "<span class=\"progress-bar__font-ode\">%s </span>", st->in_cap ? "Max Level" : "Level");
    if (st->in_cap) sb_append(sb, "<span class=\"progress-bar__font-ode\"></span></div>");
    else sb_appendf(sb, "<span class=\"progress-bar__font-ode\">%d </span></div>", lvl);
    sb_appendf(sb, "<div class=\"progress-bar__bar-wrapper\" style=\"--bar-height: 16;\">");
    sb_appendf(sb, "<div class=\"progress-bar__bar progress-bar__bar--left progress-bar__bar--border-base\" style=\"--pos1: %.14g; --pos2: %.14g; --posEnd: 1; --posStart: 0; --bar-left-width: %.14g;\">", prev, prog, bar);
    sb_appendf(sb, "<div class=\"progress-bar__bar-inner %sprogress-bar__yellow progress-bar__bar-inner--end-screen\" style=\"--bar-progress-width: %.14g; --bar-height: 14;\"></div>",
               st->lvl_up ? "progress-bar__bar-inner--leveled-up " : "", fill);
    sb_appendf(sb, "<div class=\"progress-bar__bar-inner-inner %sprogress-bar__yellow progress-bar__bar-inner-inner--end-screen\" style=\"--bar-progress-width: %.14g; --bar-height: 14;\"></div></div>",
               st->lvl_up ? "progress-bar__bar-inner-inner--leveled-up " : "", fill);
    sb_appendf(sb, "<div class=\"progress-bar__bar progress-bar__bar--middle progress-bar__bar--border-base\" style=\"--bar-middle-width: %.14g;\"></div>", LEAN_BAR_WIDTH * LEAN_BAR_MIDDLE);
    sb_appendf(sb, "<div class=\"progress-bar__bar progress-bar__bar--right progress-bar__bar--border-base\" style=\"--bar-right-width: %.14g;\"></div></div>", LEAN_BAR_WIDTH * LEAN_BAR_RIGHT);
    sb_appendf(sb, "<div class=\"progress-bar__right-text progress-bar__font-ode\">%lld / %lld</div></div></div>", (long long) st->now.xp_in_level, (long long) st->now.xp_to_next);

    sb_append(sb, "<div class=\"lean-end-screen__rewards-wrapper\"><div class=\"lean-end-screen__rewards-container\">");
    if (reward > 0) {
        sb_appendf(sb, "<div class=\"lean-end-screen__reward\"><div class=\"lean-end-screen__reward-top\"><img src=\"./img/frvr-ui/krunkies.svg\" class=\"lean-end-screen__reward-image\" style=\"image-rendering: unset;\"></div>"
                       "<div class=\"lean-end-screen__reward-bottom\"><div class=\"lean-end-screen__reward-text \">%d KR</div></div></div>", reward);
    }
    if (st->gained > 0) {
        sb_appendf(sb, "<div class=\"lean-end-screen__reward\"><div class=\"lean-end-screen__reward-top\"><img src=\"./img/frvr-ui/xp-icon.svg\" class=\"lean-end-screen__reward-image\" style=\"image-rendering: unset;\"></div>"
                       "<div class=\"lean-end-screen__reward-bottom\"><div class=\"lean-end-screen__reward-text \">%lld XP</div></div></div>", (long long) st->gained);
    }
    lean_group_html(sb, st, 0); // the claim button comes with its timer (lean_group_refresh)
    sb_append(sb, "</div></div>");
}

// the group card alone (the claim button and the claimed track's next reward)
static void lean_group_refresh(void) {
    const HtmlElem root = strike_screen_root(STRIKE_SCREEN_LEAN_END);
    HtmlElem cards[16];
    const int n = root ? html_dom_query_all(root, ".lean-end-screen__reward", cards, 16) : 0;
    if (n <= 0) return;
    LeanState st;
    lean_state(&st);
    StrBuf sb = {0};
    lean_group_html(&sb, &st, claim_visible(&st));
    char *html = sb_finish(&sb);
    html_dom_insert_html(cards[n - 1], HTML_INSERT_BEFORE_BEGIN, html);
    html_dom_remove(cards[n - 1]);
    free(html);
}

static void claim_due(void *user) {
    (void) user;
    H.claim_timer = 0;
    H.claim_shown = 1;
    lean_group_refresh();
}

static void level_due(void *user) {
    (void) user;
    H.lvl_timer = 0;
    const HtmlElem root = strike_screen_root(STRIKE_SCREEN_LEAN_END);
    HtmlElem spans[4];
    const int n = root ? html_dom_query_all(root, ".progress-bar__left-text span", spans, 4) : 0;
    char text[16];
    snprintf(text, sizeof(text), "%d ", H.lvl_now);
    if (n >= 2) html_dom_set_text(spans[1], text);
}

static void end_fill(Client *client, const int result) {
    const HtmlElem root = strike_screen_root(STRIKE_SCREEN_LEAN_END);
    if (!root) return;
    LeanState st;
    lean_state(&st);
    const Player *me = me_player(client);
    const int reward = H.scenario ? H.scn_reward : me ? me->reward : 0;
    const int ffa = !team_mode(client);
    // .frvr-end-screen__map: getGameNameAndMode(), "mode - map"
    const char *mode_name = client->game.mode && client->game.mode->config.name ? client->game.mode->config.name : "";
    const char *map_name = menu_ui()->map_name;
    char label[160];
    snprintf(label, sizeof(label), "%s%s%s", mode_name, mode_name[0] && map_name[0] ? " - " : "", map_name);

    StrBuf hs = {0}, ms = {0};
    lean_title(&hs, ffa, client->net_end_rank, result);
    sb_append(&hs, "<div class=\"frvr-end-screen__map\">");
    sb_append_esc(&hs, label);
    sb_append(&hs, "</div>");
    lean_main(&ms, &st, reward);
    char *head = sb_finish(&hs), *main_html = sb_finish(&ms);
    // the bar animates on its first draw: rebuild the header and the rewards only when they change
    if (!H.lean_head || strcmp(H.lean_head, head) || strcmp(H.lean_main, main_html)) {
        const HtmlElem header = html_dom_query(root, ".lean-end-screen__header");
        if (header) html_dom_set_html(header, head);
        // the template's bar and rewards are replaced in place: the bottom row (the podium button, the
        // timer) sits inside .lean-end-screen__main-container too, so the container's markup stays
        const HtmlElem bar = html_dom_query(root, ".progress-bar__progress-bar-holder");
        const HtmlElem rewards = html_dom_query(root, ".lean-end-screen__rewards-wrapper");
        if (bar) {
            html_dom_insert_html(bar, HTML_INSERT_BEFORE_BEGIN, main_html);
            html_dom_remove(bar);
        }
        if (rewards) html_dom_remove(rewards);
        if (claim_visible(&st)) lean_group_refresh();
        char first[16], second[24];
        const char *cls;
        lean_words(ffa, client->net_end_rank, result, first, sizeof(first), second, sizeof(second), &cls);
        printf("[strike] end screen: title \"%s%s\" level %d (%lld / %lld), reward %d KR, xp +%lld, claim %s\n", first, second, st.now.level,
               (long long) st.now.xp_in_level, (long long) st.now.xp_to_next, reward, (long long) st.gained,
               claim_visible(&st) ? "up" : "no");
        if (st.lvl_up) {
            if (H.lvl_timer) html_timer_cancel(H.lvl_timer);
            H.lvl_now = st.now.level;
            H.lvl_timer = html_timer_add(LEAN_LEVEL_MS, level_due, NULL);
        }
        free(H.lean_head);
        free(H.lean_main);
        H.lean_head = head;
        H.lean_main = main_html;
    } else {
        free(head);
        free(main_html);
    }
    // the claim button: 2 s after a level-up is on screen (WB.computeNextReward's setTimeout)
    if (st.claimable && !H.claim_shown && !H.claim_timer) H.claim_timer = html_timer_add(LEAN_CLAIM_MS, claim_due, NULL);
    // .lean-end-screen__timer: "Next round in" + the end timer's seconds (every timer packet)
    const HtmlElem timer = html_dom_query(root, ".lean-end-screen__timer__time");
    char t[16];
    time_text(client->net_end_seconds, t, sizeof(t));
    if (timer) html_dom_set_text(timer, t);
    strike_double_xp_end_fill(root); // the reward row's Double XP button (strike_double_xp.c)
    strike_banner_ad_show(root); // the bottom banner slot (strike_banner_ad.c): always supported
}

// the group's claim state and the account's track changed (strike_econ): the group card follows
static void econ_changed(void *user, StrikeEconEvent event) {
    (void) user;
    (void) event;
    if (!H.end_shown || !strike_screen_shown(STRIKE_SCREEN_LEAN_END)) return;
    LeanState st;
    lean_state(&st);
    if (st.claimable == 0) H.claim_shown = 0;
    lean_group_refresh();
}

// NET_EXT_KN_STRIKE_ROUND: the commit's XP. A round end already on screen fills in again (the XP card,
// the bar's level-up), a round end still in its banner takes the XP when it opens.
void strike_hud_round_progress(Client *client, const NetStrikeRoundPacket *round) {
    if (!strike_ui_active()) return;
    H.round = *round;
    H.round_valid = 1;
    strike_econ_refresh(NULL, NULL); // the account's XP and track claims follow the commit
    if (H.end_shown && strike_screen_shown(STRIKE_SCREEN_LEAN_END)) {
        lean_forget();
        end_fill(client, H.end_result);
    }
}

// nu(Ad, ...): LEAN_END_SCREEN over the game, its values filled
static void lean_show(Client *client, const int result) {
    H.kill_card = 0;
    strike_touch_reset();
    lean_timers_cancel();
    lean_forget();
    strike_screen_replace(STRIKE_SCREEN_LEAN_END); // nu(Ad, ...)
    end_fill(client, result);
    H.end_shown = 1;
    H.end_result = result;
    printf("[strike] end screen (%s)\n", result > 0 ? "victory" : result == 0 ? "defeat" : "draw");
}

// the victory banner (zde / Nde: .frvr-in-game-ui__victory-screen, a tB of the large size) for the
// endAnim (xe.endAnim, 2500 ms): BSe sets Mt.showVictory and hides the in-game info (displayInfoUI),
// then yR opens LEAN_END_SCREEN. Nde's words: FFA "#1 VICTORY" for the first place (gold), "MATCH
// OVER" for the rest; a team mode VICTORY (gold) / DEFEAT (place) / DRAW.
#define END_ANIM_MS 2500.0

static void banner_remove(void) {
    const HtmlElem h = holder();
    const HtmlElem banner = h ? html_dom_query(h, ".frvr-in-game-ui__victory-screen") : 0;
    if (banner) html_dom_remove(banner);
}

static void banner_cancel(void) {
    if (H.banner_timer) html_timer_cancel(H.banner_timer);
    H.banner_timer = 0;
    banner_remove();
}

static void banner_done(void *user) {
    (void) user;
    H.banner_timer = 0;
    banner_remove();
    lean_show(H.client, H.end_result);
}

static void banner_show(Client *client, const int result) {
    const HtmlElem h = holder();
    if (!h) {
        lean_show(client, result);
        return;
    }
    const char *texts[2] = {NULL, NULL}, *cls = NULL;
    int count = 1;
    if (!team_mode(client)) {
        if (client->net_end_rank == 1) {
            texts[0] = "#1";
            texts[1] = " VICTORY";
            count = 2;
            cls = "frvr-in-game-ui__victory-text--gold";
        } else {
            texts[0] = "MATCH OVER";
        }
    } else if (result > 0) {
        texts[0] = "VICTORY";
        cls = "frvr-in-game-ui__victory-text--gold";
    } else if (result == 0) {
        texts[0] = "DEFEAT";
        cls = "frvr-in-game-ui__victory-text--place";
    } else {
        texts[0] = "DRAW";
    }
    StrBuf sb = {0};
    sb_append(&sb, "<div class=\"frvr-in-game-ui__victory-screen\"><div class=\"frvr-in-game-ui__regular-victory\">"
                   "<img src=\"./img/victory-screen-bg.png\" class=\"frvr-in-game-ui__regular-victory--large \">");
    sb_append(&sb, "<div class=\"frvr-in-game-ui__victory-text-holder\">");
    for (int i = 0; i < count; i++) victory_copy(&sb, 1, NULL, texts[i], "--large");
    sb_append(&sb, "</div><div class=\"frvr-in-game-ui__victory-text-holder\">");
    for (int i = 0; i < count; i++) victory_copy(&sb, 0, i == 0 ? cls : NULL, texts[i], "--large");
    sb_append(&sb, "</div></div></div>");
    char *html = sb_finish(&sb);

    // the in-game HUD steps aside (displayInfoUI = false): only the banner is drawn over the game
    H.kill_card = 0;
    strike_touch_reset();
    unrender();
    const HtmlElem anchor = game_anchor(h);
    if (anchor) html_dom_insert_html(anchor, HTML_INSERT_BEFORE_BEGIN, html);
    else html_dom_insert_html(h, HTML_INSERT_BEFORE_END, html);
    free(html);
    printf("[strike] end banner \"%s%s\"\n", texts[0], count > 1 ? texts[1] : "");
    H.banner_timer = html_timer_add(END_ANIM_MS, banner_done, NULL);
}

void strike_hud_end(Client *client, const int result) {
    if (!strike_ui_active()) return;
    H.client = client;
    H.end_result = result;
    // FRVR's endGame pushes LEAN_END_SCREEN once (nu(Ad, ...)); its end timer only updates the
    // screen's values (endTimer). The end timer's packets repeat this every second, so once this round
    // end was shown, a screen the player left (Home, Settings over the menu) stays left: only the
    // values of a round end that is still on the stack follow (MATCH_DETAILS_SCREEN pushed over it).
    if (H.end_shown) {
        if (strike_screen_shown(STRIKE_SCREEN_LEAN_END)) end_fill(client, result);
        return;
    }
    if (H.banner_timer) return; // the endAnim runs: its end opens the lean screen
    // --render-scenario: the lean screen at once unless the fixture asks for the banner (endBanner)
    if (H.scenario && !H.scn_banner) {
        lean_show(client, result);
        return;
    }
    banner_show(client, result);
}

void strike_hud_end_hide(Client *client) {
    (void) client;
    if (!strike_ui_active()) return;
    banner_cancel();
    if (!H.end_shown) return;
    H.end_shown = 0;
    lean_timers_cancel();
    lean_forget();
    if (strike_screen_top() == STRIKE_SCREEN_LEAN_END) strike_screen_clear();
}

int strike_hud_end_up(void) { return H.end_shown; }

// The menu closed over a round end that is still on (strike_ui_menu_open(0) cleared the screens): the
// round end comes back over the game view, as FRVR's end screen stays on its back stack.
void strike_hud_end_restore(void) {
    if (!H.end_shown || !H.client || strike_screen_shown(STRIKE_SCREEN_LEAN_END)) return;
    lean_forget();
    strike_screen_push(STRIKE_SCREEN_LEAN_END);
    end_fill(H.client, H.end_result);
}

// ---------------------------------------------------------------- game events

static void log_expire(void *user) {
    (void) user;
    // the oldest row's setTimeout: Mt.killLogStack.shift()
    if (H.log_count <= 0) return;
    memmove(&H.log[0], &H.log[1], sizeof(H.log[0]) * (size_t) (H.log_count - 1));
    H.log_count--;
    H.log_dirty = 1;
}

static void killfeed_add(Client *client, const char *killer, const int killer_is_you, const char *victim, const int victim_is_you,
                         const char *icon, const int headshot, const int thrown, const int wallbang);

void strike_hud_killfeed(Client *client, const char *killer, const int killer_is_you, const char *victim, const int victim_is_you,
                         const Weapon *weapon, const int headshot, const int thrown, const int wallbang, const int hidden) {
    if (!strike_ui_active()) return;
    // NET_KILL_HIDE_FEED ("3" hideFeed, game.deob.js:220629; hud_ui.c hud_killfeed_row): no row for this kill,
    // the rows already up run out their 10 s as before
    if (hidden) return;
    killfeed_add(client, killer, killer_is_you, victim, victim_is_you, weapon ? weapon->icon : NULL, headshot, thrown, wallbang);
}

static void killfeed_add(Client *client, const char *killer, const int killer_is_you, const char *victim, const int victim_is_you,
                         const char *icon, const int headshot, const int thrown, const int wallbang) {
    H.client = client;
    if (H.log_count >= KILL_LOG_MAX) {
        if (H.log[0].timer) html_timer_cancel(H.log[0].timer);
        memmove(&H.log[0], &H.log[1], sizeof(H.log[0]) * (KILL_LOG_MAX - 1));
        H.log_count--;
    }
    KillLogRow *r = &H.log[H.log_count++];
    memset(r, 0, sizeof(*r));
    snprintf(r->killer, sizeof(r->killer), "%s", killer ? killer : "");
    snprintf(r->victim, sizeof(r->victim), "%s", victim ? victim : "");
    r->killer_is_you = killer_is_you;
    r->victim_is_you = victim_is_you;
    r->headshot = headshot;
    r->thrown = thrown;
    r->wallbang = wallbang;
    if (icon && icon[0]) snprintf(r->weapon, sizeof(r->weapon), "./cdn/textures/weapons/%s.png", icon);
    r->timer = html_timer_add(KILL_LOG_MS, log_expire, NULL);
    H.log_dirty = 1;
}

void strike_hud_death(Client *client, const HudKillCard *card, const Player *killer) {
    if (!strike_ui_active()) return;
    H.client = client;
    // Mt.killCardInfo: pfp VH(classIndex), nickname, weapon "[name]", lvl, score
    H.killed_by = card && card->killer && card->killer[0];
    if (H.killed_by) {
        snprintf(H.kc_name, sizeof(H.kc_name), "%s", card->killer);
        snprintf(H.kc_weapon, sizeof(H.kc_weapon), "%s[%s]", card->thrown ? "Thrown " : "", card->weapon ? card->weapon : "");
        // the weapon's colour: the killer's skin rarity (weapon_color), else FRVR's rgba(255,255,255,0.7)
        snprintf(H.kc_color, sizeof(H.kc_color), "%s", card->weapon_color && card->weapon_color[0] ? card->weapon_color : "rgba(255,255,255,0.7)");
        class_icon(killer ? killer->class_index : card->class_index, H.kc_pfp, sizeof(H.kc_pfp));
        // the killer's Country Flag (FRVR killCardInfo.flag): the upper-case code names img/flags/3x2/<CODE>.svg
        H.kc_flag[0] = H.kc_flag[1] = H.kc_flag[2] = 0;
        if (card->flag_code && strlen(card->flag_code) == 2) {
            H.kc_flag[0] = (char) toupper((unsigned char) card->flag_code[0]);
            H.kc_flag[1] = (char) toupper((unsigned char) card->flag_code[1]);
        }
        H.kc_level = card->level > 0 ? card->level : 1;
        H.kc_score = card->score;
    }
    H.kill_card = 1; // Mt.showKillCard (the controls go: strike_touch_tick sees the dead player)
    strike_touch_reset();
}

// After xe.deathDelay: Cg() -> goToPlay (game_events.c client_game_events_tick calls this
// instead of reopening the menu). The kill card goes with the respawn.
void strike_hud_respawn(Client *client) {
    if (!strike_ui_active()) return;
    H.kill_card = 0;
    client_enter_game(client);
}

// A hit on a victim (game_events.c NET_PACKET_HIT under Strike): FRVR's addDamageNumber, when showDMG is on.
// key: the victim's uid, STRIKE_DMG_KEY_AI for a bot; (x, feet_y, z) the victim's feet. The mode is the Display
// tab's damageNumberDisplayMode, dmgType "Rounded" rounds up (ceil) as FRVR's Qce does.
void strike_hud_damage_number(Client *client, const int key, const float x, const float feet_y, const float z, const double value,
                              const int crit) {
    (void) client;
    const ClientRenderSettings *rs = client_render_settings();
    if (!H.root || !rs->show_dmg || value == 0.0) return; // no HUD (menu open, end screen): nothing to draw on
    const double shown = rs->dmg_type == 0 ? ceil(value) : value;
    const float dir = (float) (((double) rand() / RAND_MAX) * 2.0 * M_PI - M_PI); // Legacy's randFloat(-PI, PI)
    strike_damage_add(&FX.dmg, rs->damage_number_mode, key, x, feet_y, z, shown, crit, game_constants.player_height, dir);
}

void strike_hud_install(void) {
    strike_econ_listen(econ_changed, NULL); // the round end's claim follows the track (strike_econ)
}

void strike_hud_reset(void) {
    unrender();
    banner_cancel();
    for (int i = 0; i < H.log_count; i++) {
        if (H.log[i].timer) html_timer_cancel(H.log[i].timer);
    }
    H.log_count = 0;
    H.kill_card = 0;
    H.end_shown = 0;
    H.round_valid = 0;
    lean_timers_cancel();
    lean_forget();
}

int strike_hud_spectating(void) { return spectate_active(); }

// The spectate actions (the panel's and the kill card's buttons, strike_hud_pointer). Spectating
// is spectate.c's: the request is what the server reads on our next ENT (client_enter_game).
int strike_hud_action(const char *name) {
    if (!strcmp(name, "spectate")) {
        spectate_set_requested(1); // the kill card's Spectate: the respawn (strike_hud_respawn) enters spectating
    } else if (!strcmp(name, "spectate-prev") || !strcmp(name, "spectate-next")) {
        if (H.client) spectate_mode(H.client, !strcmp(name, "spectate-next") ? 1 : -1);
    } else if (!strcmp(name, "spectate-play")) {
        // leave spectating: the request off, one ENT that spawns us (client_enter_game's leave path)
        spectate_set_requested(0);
        if (H.client) client_enter_game(H.client);
    } else {
        return 0;
    }
    return 1;
}

// A press at a page point (CSS px) on one of the HUD's buttons: runs its action, 1 when it hit one.
// strike_touch.c calls it before the touch controls (the HUD's buttons sit where the controls are not).
int strike_hud_pointer(const float x, const float y) {
    static const struct { const char *sel, *action; } BUTTONS[] = {
        {".frvr-in-game-ui__kill-card-spectate", "spectate"},
        {".frvr-in-game-ui__spectate-prev", "spectate-prev"},
        {".frvr-in-game-ui__spectate-next", "spectate-next"},
        {".frvr-in-game-ui__spectate-play", "spectate-play"},
    };
    if (!H.root) return 0;
    for (size_t i = 0; i < sizeof(BUTTONS) / sizeof(BUTTONS[0]); i++) {
        const HtmlElem el = part(BUTTONS[i].sel);
        HtmlRect r;
        if (!el || !html_dom_get_rect(el, &r) || r.w <= 0.0f || r.h <= 0.0f) continue;
        if (x < r.x || x > r.x + r.w || y < r.y || y > r.y + r.h) continue;
        return strike_hud_action(BUTTONS[i].action);
    }
    return 0;
}

void strike_hud_tick(Client *client) {
    H.client = client;
    const int spec = spectate_active();
    // Mt.inGameUI.visible: in a game, the menu closed (Yh(true)), not the end screen; spectating has no
    // player of ours, the spectate panel is the HUD. The victory banner (endAnim) holds the HUD back too.
    const int want = client && client->in_game && (client->me || spec) && !html_ui_is_menu_open() && !H.end_shown && !H.banner_timer;
    // Mt.showMiniScoreBoard: the scoreboard key held (ui_controls.c: not while typing); a dead player
    // sees the kill card instead (Mt.displayInfoUI is false then)
    H.board = want && !H.kill_card && (H.scn_board || ui_controls_scoreboard_held());
    if (H.root && H.rendered_spec != spec) unrender(); // spectating began or ended: the panel and the health swap
    if (want && !H.root) render(client);
    else if (!want && H.root) unrender();
    if (H.root) update(client);
    if (H.root) fx_tick(client); // the damage numbers rise and fade; the crosshair follows the player
    // spawned: Yh(true) hides the card; spectating has no card (the panel replaces it)
    if (H.kill_card && !H.scenario && client && ((client->me && client->me->active) || spec)) H.kill_card = 0;
}

static const char *sjs(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static int sjn(const cJSON *o, const char *k, const int fallback) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valueint : cJSON_IsBool(v) ? cJSON_IsTrue(v) : fallback;
}

void strike_hud_scenario(Client *client, const cJSON *st) {
    H.scenario = 1;
    H.scn_health = sjn(st, "health", 0);
    H.scn_max_health = sjn(st, "maxHealth", 0);
    // the round end's values (no players in a scenario): the place (1 = first), the KR of the match
    // (player.reward) and the end timer's seconds
    H.scn_reward = sjn(st, "endReward", 0);
    H.scn_banner = sjn(st, "endBanner", 0);
    H.scn_claim = sjn(st, "claim", 0); // the claim button up at once
    const cJSON *round = cJSON_GetObjectItemCaseSensitive(st, "round"); // {xp, gained}: the round's XP
    if (cJSON_IsObject(round)) {
        H.round.xp = sjn(round, "xp", 0);
        H.round.xp_gained = sjn(round, "gained", 0);
        H.round_valid = 1;
    }
    if (client) {
        client->net_end_rank = sjn(st, "endRank", 1);
        client->net_end_seconds = sjn(st, "endSeconds", 0);
    }
    const cJSON *feed = cJSON_GetObjectItemCaseSensitive(st, "killfeed");
    const cJSON *row;
    cJSON_ArrayForEach(row, feed) {
        killfeed_add(client, sjs(row, "killer"), sjn(row, "killerIsYou", 0), sjs(row, "victim"), sjn(row, "victimIsYou", 0), sjs(row, "weapon"),
                     sjn(row, "headshot", 0), sjn(row, "thrown", 0), sjn(row, "wallbang", 0));
    }
    const cJSON *kc = cJSON_GetObjectItemCaseSensitive(st, "killcard");
    if (cJSON_IsObject(kc)) {
        HudKillCard card = {0};
        card.killer = sjs(kc, "killer");
        card.weapon = sjs(kc, "weapon");
        card.weapon_color = sjs(kc, "weaponColor");
        card.thrown = sjn(kc, "thrown", 0);
        card.level = sjn(kc, "level", 1);
        card.score = sjn(kc, "score", 0);
        card.class_index = sjn(kc, "classIndex", 0);
        strike_hud_death(client, &card, NULL);
    }
    // state.strike.scoreboard: the scoreboard key held; state.strike.board: [{name, team, me, classIndex, score, kills, deaths}]
    H.scn_board = sjn(st, "scoreboard", 0);
    const cJSON *board = cJSON_GetObjectItemCaseSensitive(st, "board");
    cJSON_ArrayForEach(row, board) {
        if (H.scn_row_count >= BOARD_ROWS_MAX) break;
        BoardRow *r = &H.scn_rows[H.scn_row_count++];
        const char *name = sjs(row, "name");
        memset(r, 0, sizeof(*r));
        snprintf(r->name, sizeof(r->name), "%s", name ? name : "");
        r->team = sjn(row, "team", 0);
        r->is_you = sjn(row, "me", 0);
        r->class_index = sjn(row, "classIndex", 0);
        r->score = sjn(row, "score", 0);
        r->kills = sjn(row, "kills", 0);
        r->deaths = sjn(row, "deaths", 0);
    }
    H.scn_cross = sjn(st, "crosshair", 0); // state.strike.crosshair: the Dynamic crosshair at rest (none by default)
    const cJSON *end = cJSON_GetObjectItemCaseSensitive(st, "end");
    if (cJSON_IsNumber(end)) strike_hud_end(client, end->valueint);
    printf("[strike] in-game scenario: %d kill-feed rows, kill card %d, scoreboard %d (%d rows)\n", H.log_count, H.kill_card, H.scn_board,
           H.scn_row_count);
}
