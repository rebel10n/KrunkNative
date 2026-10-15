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
//   jde / Kde     .frvr-in-game-ui__kill-log-container: Ybe = 3 rows, each dropped 10 s after it came;
//                 killer / victim coloured by AF (friend-colors--you / --enemy / --teammate)
//   Yde           .frvr-in-game-ui__kill-card-container: "Killed By" + the killer's card, or "You Died"
// The death: Mt.showKillCard, the mobile controls hidden, and xe.deathDelay later Cg() (goToPlay):
// a Strike player respawns on their own, no menu in between (strike_hud_death_tick).
// The round end: LEAN_END_SCREEN (WB, the template LEAN_END_SCREEN.html) over the game.
#include <strike_ui.h>
#include <html_ui.h>
#include <ui_logic.h>
#include <shared.h>
#include <modes.h>
#include <weapons.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cJSON.h>

#define KILL_LOG_MAX 3          // Ybe
#define KILL_LOG_MS 10000.0     // a row's setTimeout
#define HEALTH_BARS 6           // Sx
#define KILL_NAME_MAX 48

typedef struct {
    char killer[KILL_NAME_MAX], victim[KILL_NAME_MAX];
    char weapon[96];            // icon src ("" none)
    int killer_is_you, victim_is_you, killer_team, victim_team, headshot;
    int timer;
} KillLogRow;

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
    char kc_name[KILL_NAME_MAX], kc_weapon[96], kc_pfp[96];
    int kc_level, kc_score;
    // last rendered values (Mithril's diff)
    char last_score[160], last_health[64], last_timer[64], last_reload[32];
    int last_card;
    int end_shown;
    // --render-scenario: a pinned card / health (the reference states hold them while the sandbox player lives)
    int scenario, scn_health, scn_max_health;
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
        if (r->weapon[0]) sb_appendf(sb, "<div class=\"frvr-in-game-ui__kill-log-img\"><img src=\"%s\"></div>", r->weapon);
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
        sb_append(sb, "</div><div class=\"frvr-in-game-ui__kill-card-weapon\" style=\"color: rgb(255, 255, 255);\">");
        sb_append_esc(sb, H.kc_weapon);
        sb_appendf(sb, "</div></div><div class=\"frvr-in-game-ui__kill-card-lvl-flag\"><div class=\"frvr-in-game-ui__kill-card-lvl\">LVL %d</div></div>", H.kc_level);
        sb_appendf(sb, "<div class=\"frvr-in-game-ui__kill-card-score\">+ %d</div></div>", H.kc_score);
    }
    sb_append(sb, "</div>");
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

static void render(Client *client) {
    const HtmlElem h = holder();
    if (!h) return;
    StrBuf sb = {0};
    sb_append(&sb, "<div id=\"InGameUIMobile\" class=\"frvr-in-game-ui\">");
    view_scorebar(client, &sb);
    view_health(client, &sb);
    view_reload(client, &sb);
    view_kill_log(client, &sb);
    if (H.kill_card) view_kill_card(&sb);
    sb_append(&sb, "</div>");
    char *html = sb_finish(&sb);
    const HtmlElem anchor = game_anchor(h);
    if (anchor) html_dom_insert_html(anchor, HTML_INSERT_BEFORE_BEGIN, html);
    else html_dom_insert_html(h, HTML_INSERT_BEFORE_END, html);
    free(html);
    H.root = html_dom_by_id("InGameUIMobile");
    H.log_dirty = 0;
    H.last_card = H.kill_card;
}

static void unrender(void) {
    if (H.root) html_dom_remove(H.root);
    H.root = 0;
    H.last_score[0] = H.last_health[0] = H.last_timer[0] = H.last_reload[0] = 0;
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
    if (H.log_dirty) {
        H.log_dirty = 0;
        view_kill_log(client, &sb);
        html = sb_finish(&sb);
        replace_part(".frvr-in-game-ui__kill-log-container", html);
        free(html);
    }
    if (H.kill_card != H.last_card) {
        H.last_card = H.kill_card;
        const HtmlElem old = part(".frvr-in-game-ui__kill-card-container");
        if (old) html_dom_remove(old);
        if (H.kill_card) {
            view_kill_card(&sb);
            html = sb_finish(&sb);
            html_dom_insert_html(H.root, HTML_INSERT_BEFORE_END, html);
            free(html);
        }
    }
}

// ---------------------------------------------------------------- the round end (LEAN_END_SCREEN)

static void end_fill(Client *client, const int result) {
    const HtmlElem root = strike_screen_root(STRIKE_SCREEN_LEAN_END);
    if (!root) return;
    // tB: the victory title (#1 VICTORY / DEFEAT / DRAW) on the lean screen
    const char *state = result > 0 ? "--victory" : result == 0 ? "--defeat" : "--draw";
    HtmlElem imgs[8];
    const int n = html_dom_query_all(root, ".lean-end-screen__victory-defeat img", imgs, 8);
    for (int i = 0; i < n; i++) {
        html_dom_set_class(imgs[i], "lean-end-screen__victory-defeat--victory", result > 0);
        html_dom_set_class(imgs[i], "lean-end-screen__victory-defeat--defeat", result == 0);
        html_dom_set_class(imgs[i], "lean-end-screen__victory-defeat--draw", result < 0);
    }
    (void) state;
    // rank: the place among the players by score (legacyData.rank "#N")
    const Player *me = me_player(client);
    int rank = 1;
    if (me) {
        for (size_t i = 0; i < client->game.player_count; i++) {
            const Player *p = client->game.players[i];
            if (p && p != me && p->score > me->score) rank++;
        }
    }
    // tB textArray ["#N", " VICTORY"]: a stroke copy and a gold / plain copy of each part
    HtmlElem texts[8];
    const int tn = html_dom_query_all(root, ".frvr-in-game-ui__victory-text", texts, 8);
    char first[16];
    snprintf(first, sizeof(first), "#%d", rank);
    const char *second = result > 0 ? " VICTORY" : result == 0 ? " DEFEAT" : " DRAW";
    for (int i = 0; i < tn; i++) html_dom_set_text(texts[i], i % 2 == 0 ? first : second);
    // .frvr-end-screen__map: the map's name
    const HtmlElem map = html_dom_query(root, ".frvr-end-screen__map");
    if (map) html_dom_set_text(map, menu_ui()->map_name[0] ? menu_ui()->map_name : "");
}

void strike_hud_end(Client *client, const int result) {
    if (!strike_ui_active()) return;
    H.client = client;
    // the end timer's packets repeat this every second: the round end is up already (with the
    // screens pushed over it, MATCH_DETAILS_SCREEN), so only its values follow
    if (H.end_shown && strike_screen_shown(STRIKE_SCREEN_LEAN_END)) {
        end_fill(client, result);
        return;
    }
    H.kill_card = 0;
    strike_touch_reset();
    strike_screen_replace(STRIKE_SCREEN_LEAN_END); // nu(Ad, ...)
    end_fill(client, result);
    H.end_shown = 1;
    printf("[strike] end screen (%s)\n", result > 0 ? "victory" : result == 0 ? "defeat" : "draw");
}

void strike_hud_end_hide(Client *client) {
    (void) client;
    if (!strike_ui_active() || !H.end_shown) return;
    H.end_shown = 0;
    if (strike_screen_top() == STRIKE_SCREEN_LEAN_END) strike_screen_clear();
}

int strike_hud_end_up(void) { return H.end_shown; }

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
                         const char *icon, const int headshot);

void strike_hud_killfeed(Client *client, const char *killer, const int killer_is_you, const char *victim, const int victim_is_you,
                         const Weapon *weapon, const int headshot) {
    if (!strike_ui_active()) return;
    killfeed_add(client, killer, killer_is_you, victim, victim_is_you, weapon ? weapon->icon : NULL, headshot);
}

static void killfeed_add(Client *client, const char *killer, const int killer_is_you, const char *victim, const int victim_is_you,
                         const char *icon, const int headshot) {
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
        class_icon(killer ? killer->class_index : card->class_index, H.kc_pfp, sizeof(H.kc_pfp));
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

void strike_hud_install(void) {}

void strike_hud_reset(void) {
    unrender();
    for (int i = 0; i < H.log_count; i++) {
        if (H.log[i].timer) html_timer_cancel(H.log[i].timer);
    }
    H.log_count = 0;
    H.kill_card = 0;
    H.end_shown = 0;
}

void strike_hud_tick(Client *client) {
    H.client = client;
    // Mt.inGameUI.visible: in a game, the menu closed (Yh(true)), not the end screen
    const int want = client && client->in_game && client->me && !html_ui_is_menu_open() && !H.end_shown;
    if (want && !H.root) render(client);
    else if (!want && H.root) unrender();
    if (H.root) update(client);
    if (H.kill_card && !H.scenario && client && client->me && client->me->active) H.kill_card = 0; // spawned: Yh(true) hides the card
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
    const cJSON *feed = cJSON_GetObjectItemCaseSensitive(st, "killfeed");
    const cJSON *row;
    cJSON_ArrayForEach(row, feed) {
        killfeed_add(client, sjs(row, "killer"), sjn(row, "killerIsYou", 0), sjs(row, "victim"), sjn(row, "victimIsYou", 0), sjs(row, "weapon"),
                     sjn(row, "headshot", 0));
    }
    const cJSON *kc = cJSON_GetObjectItemCaseSensitive(st, "killcard");
    if (cJSON_IsObject(kc)) {
        HudKillCard card = {0};
        card.killer = sjs(kc, "killer");
        card.weapon = sjs(kc, "weapon");
        card.level = sjn(kc, "level", 1);
        card.score = sjn(kc, "score", 0);
        card.class_index = sjn(kc, "classIndex", 0);
        strike_hud_death(client, &card, NULL);
    }
    const cJSON *end = cJSON_GetObjectItemCaseSensitive(st, "end");
    if (cJSON_IsNumber(end)) strike_hud_end(client, end->valueint);
}
