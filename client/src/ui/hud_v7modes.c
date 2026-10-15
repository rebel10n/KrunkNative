// The v7.1.4 mode HUD pieces map-D's mode mirror (client/src/ui/hud_modes.c, hud_modes_v7())
// and map-B's world packets (client/src/world_events.c) feed, on the ui/index.html DOM:
//
//   #roundsDisplay   io_startGame / f1995: inline-block when config.gamRounds > 1 and not
//                    spectating; #roundsVal "gamRounds - roundCount / gamRounds" (initGame),
//                    #roundTSc1 / #roundTSc2 the teams' round wins.
//   #krTagHolder     io_updateKrTags("krt"): one .killStreakItem with "x<dogTags>", red once
//                    the carried tags reach mode.tagHoldLimit (textures/sprays/114.png).
//   #bhHolder        initGame: inline-block in bighead modes; f2014 sizes its head
//                    calc(40px * headMlt) from the local player's headMlt.
//   #speedRunHolder  f1889 + mode.custUI (gameModes/parkour.js, race.js) while game.doLogTime:
//                    #weapHolder / #bottomRight hidden, #speedRunReset when mode.allowReset,
//                    the "Point N" list (config.maxSpdRChck rows), "Any% 0/<zones>" and
//                    "00:00.0"; Game.logTime ("gt") fills the rows and moves .sriAct, the
//                    speed-run "t" (ownTime clock / final time) writes #speedRunTimer;
//                    updateKeybindUI writes #speedRunRstKey / #speedRunRstLKey.
//
// hud_ui.c's visibility passes (toggleControlUI / toggleGameUI) show #weapHolder / #bottomRight
// and hide #bhHolder again; `layout_epoch` changes then, and the pieces here are re-applied after
// them, as v7 runs f1889 after toggleGameUI on every spawn.
#include <ui_logic.h>
#include <hud_modes.h>
#include <world_events.h>
#include <spectate.h>
#include <html_dom.h>
#include <modes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define MAX_SPD_R_CHCK 6 // config.maxSpdRChck

static struct {
    int layout_epoch;      // hud_ui.c's last visibility pass we followed
    // speed run
    int speed_run;         // #speedRunHolder shown (custUI ran)
    int list_built;        // custUI(..., true): the list of this round is in #speedRunList
    int zone_count;        // map.manager.scorezones.length
    int seen_has, seen_time, seen_log, seen_kind; // the last NET_PACKET_LOG_TIME handled
    int own_clock;         // a speed-run "t" arrived this round: #speedRunTimer is the player's clock
    char timer[32];        // #speedRunTimer as written
    char key_reset[48], key_last[48];
    // rounds
    int rounds_shown;
    char rounds_val[32];
    int round_wins[2];
    // KR tags
    int tags_shown, tags, tags_full;
    // big head
    int bighead_shown;
    float head_mlt;
} g_v7;

static void v7_display(const char *id, const char *value) {
    const HtmlElem el = html_dom_by_id(id);
    if (el) html_dom_set_display(el, value);
}

static void v7_text(const char *id, const char *text) {
    const HtmlElem el = html_dom_by_id(id);
    if (el) html_dom_set_text(el, text);
}

static void v7_class(const char *id, const char *cls) {
    const HtmlElem el = html_dom_by_id(id);
    if (el) html_dom_set_attr(el, "class", cls);
}

// INIT (client_game_events_reset): v7 initGame empties #krTagHolder and rebuilds the custUI list.
void hud_v7modes_on_init(void) {
    const HtmlElem tags = html_dom_by_id("krTagHolder");
    if (tags) html_dom_set_html(tags, "");
    g_v7.list_built = 0;
    g_v7.own_clock = 0;
    g_v7.seen_has = 0;
    g_v7.timer[0] = 0;
    g_v7.tags_shown = 0;
    g_v7.tags = g_v7.tags_full = -1;
    g_v7.head_mlt = -1.0f;
    g_v7.rounds_val[0] = 0;
    g_v7.round_wins[0] = g_v7.round_wins[1] = -1;
}

// UTILS.getTime(t, ms, showHour, 1000, showDay, 2) as logTime / the "t" senders call it:
// "MM:SS.<hundredths without the 0.>" with hours (and days) once the time reaches them.
void hud_v7modes_format_time(const double t, const int with_ms, char *out, const size_t size) {
    const double time = t < 0.0 ? 0.0 : t;
    const int show_hour = time >= 3600000.0, show_day = time >= 86400000.0;
    char ms[16] = "0";
    double frac = fmod(time, 1000.0) / 1000.0;
    frac = round(frac * 100.0) / 100.0; // Number.round(2)
    if (frac != 0.0) {
        char num[24];
        snprintf(num, sizeof(num), "%g", frac); // "0.25", "0.5", "1"
        snprintf(ms, sizeof(ms), "%s", !strncmp(num, "0.", 2) ? num + 2 : num); // replace("0.", "")
    }
    const long long total = (long long) time;
    const int seconds = (int) (total / 1000 % 60), minutes = (int) (total / 60000 % 60);
    const int hours = (int) (total / 3600000 % 24);
    const long long days = total / 86400000;
    char day[24] = "", hour[16] = "";
    if (show_day && days) snprintf(day, sizeof(day), "%s%lld:", days < 10 ? "0" : "", days);
    if (show_hour && (hours || days)) snprintf(hour, sizeof(hour), "%s%d:", show_day && days && hours < 10 ? "0" : "", hours);
    snprintf(out, size, "%s%s%02d:%02d%s%s", day, hour, minutes, seconds, with_ms ? "." : "", with_ms ? ms : "");
}

static int score_zone_count(const Game *game) {
    int count = 0;
    for (size_t i = 0; game->map && i < game->map->object_count; i++) count += game->map->objects[i]->score_zone;
    return count;
}

// game.doLogTime = mode.logTime && config.logTim
static int do_log_time(const Game *game) {
    if (!game->ready || !game->mode || !game->mode->config.log_time) return 0;
#ifdef GAME_CONFIG_HAS_LOG_TIME
    if (!game->config.log_time) return 0;
#endif
    return 1;
}

static void speed_run_item(char *out, const size_t size, const int point, const int active) {
    snprintf(out, size, "<div class='speedRunItem%s' id='spdRunI%d'>Point %d<span class='speedRunITim' id='spdRunIVal%d'>0:00</span></div>",
             active ? " sriAct" : "", point, point, point);
}

// mode.custUI(Game, config, true): the Point rows, "Any%" and the clock
static void speed_run_build(const Game *game) {
    g_v7.zone_count = score_zone_count(game);
    char list[2048] = "", item[256];
    size_t len = 0;
    for (int i = 0; i < g_v7.zone_count && i < MAX_SPD_R_CHCK; i++) {
        speed_run_item(item, sizeof(item), i + 1, i == 0);
        len += (size_t) snprintf(list + len, sizeof(list) - len, "%s", item);
        if (len >= sizeof(list)) break;
    }
    HtmlElem el = html_dom_by_id("speedRunList");
    if (el) html_dom_set_html(el, list[0] ? list : "<div>No Score Zones</div>");
    char info[512];
    snprintf(info, sizeof(info), "Any%% <span style='float:right;color:#fff'><span id='spdrnZC'  style='color:#fff'>0</span>/%d"
                                 "<i class='material-icons' style='color:#fff;font-size:30px;margin-left:10px'>flag</i></span>", g_v7.zone_count);
    if ((el = html_dom_by_id("speedRunInfo"))) html_dom_set_html(el, info);
    snprintf(g_v7.timer, sizeof(g_v7.timer), "00:00.0");
    v7_text("speedRunTimer", g_v7.timer);
    g_v7.list_built = 1;
}

// Game.logTime(time, zoneIndex): the row's time, the zone count, the rolling list, .sriAct
static void speed_run_log(const int time_ms, const int zone) {
    char id[32], text[48];
    snprintf(id, sizeof(id), "spdRunIVal%d", zone);
    hud_v7modes_format_time(time_ms, 1, text, sizeof(text));
    v7_text(id, text);
    snprintf(text, sizeof(text), "%d", zone);
    v7_text("spdrnZC", text);
    if (zone >= MAX_SPD_R_CHCK) {
        const int next = zone + 1;
        if (next < g_v7.zone_count) { // v7's bound: the last point is never appended
            snprintf(id, sizeof(id), "spdRunI%d", (zone - MAX_SPD_R_CHCK > 0 ? zone - MAX_SPD_R_CHCK : 0) + 1);
            HtmlElem old = html_dom_by_id(id);
            if (old) html_dom_remove(old);
            const HtmlElem list = html_dom_by_id("speedRunList");
            char item[256];
            speed_run_item(item, sizeof(item), next, 0);
            if (list) html_dom_insert_html(list, HTML_INSERT_BEFORE_END, item);
        }
    }
    snprintf(id, sizeof(id), "spdRunI%d", zone + 1);
    if (html_dom_by_id(id)) {
        v7_class(id, "speedRunItem sriAct");
        snprintf(id, sizeof(id), "spdRunI%d", zone);
        v7_class(id, "speedRunItem");
    }
}

// updateKeybindUI("reset" / "resetLast"): "[<key>]"
static void speed_run_keys(void) {
    char name[32], text[48];
    snprintf(text, sizeof(text), "[%s]", ui_controls_key_name(ui_controls_v7_bind("reset", -1, 0), name, sizeof(name)));
    if (strcmp(text, g_v7.key_reset) != 0) {
        snprintf(g_v7.key_reset, sizeof(g_v7.key_reset), "%s", text);
        v7_text("speedRunRstKey", text);
    }
    snprintf(text, sizeof(text), "[%s]", ui_controls_key_name(ui_controls_v7_bind("resetLast", -1, 0), name, sizeof(name)));
    if (strcmp(text, g_v7.key_last) != 0) {
        snprintf(g_v7.key_last, sizeof(g_v7.key_last), "%s", text);
        v7_text("speedRunRstLKey", text);
    }
}

static void speed_run_tick(const Client *client, const int on, const int relayout) {
    const Game *game = &client->game;
    const int active = on && do_log_time(game) && !spectate_active();
    if (active != g_v7.speed_run || (active && relayout)) {
        // Leaving a speed-run mode (Parkour -> FFA, the offline game reloads and respawns in one
        // frame): v7's init toggleGameUI(true) brings #weapHolder / #bottomRight back; here the
        // HUD's visibility pass does (with the death / menu state it knows).
        if (!active && g_v7.speed_run) hud_ui_relayout();
        g_v7.speed_run = active;
        v7_display("speedRunHolder", active ? "block" : "none"); // f1889: none, then custUI
        if (active) {
            v7_display("weapHolder", "none");
            v7_display("bottomRight", "none");
            v7_display("speedRunReset", game->mode->config.allow_reset ? "inline-block" : "none");
        }
    }
    if (!active) return;
    if (!g_v7.list_built) speed_run_build(game);
    speed_run_keys();

    int time_ms = 0, time_log = 0, kind = 0, show_ms = 0;
    const int has = client_world_log_time(&time_ms, &time_log, &kind, &show_ms);
    if (has && (!g_v7.seen_has || time_ms != g_v7.seen_time || time_log != g_v7.seen_log || kind != g_v7.seen_kind)) {
        g_v7.seen_has = 1;
        g_v7.seen_time = time_ms;
        g_v7.seen_log = time_log;
        g_v7.seen_kind = kind;
        if (kind == NET_LOG_TIME_ZONE) {
            speed_run_log(time_ms, time_log);
        } else { // "t": the ownTime clock (showMS) or the final time (always with ms)
            g_v7.own_clock = 1;
            hud_v7modes_format_time(time_ms, kind == NET_LOG_TIME_FINISH || show_ms, g_v7.timer, sizeof(g_v7.timer));
            v7_text("speedRunTimer", g_v7.timer);
        }
    }
    // io_updateTimer writes the round clock into #speedRunTimer too (Race's timer)
    if (!g_v7.own_clock && client->net_round_seconds >= 0) {
        char text[32];
        snprintf(text, sizeof(text), "%02d:%02d", client->net_round_seconds / 60, client->net_round_seconds % 60);
        if (strcmp(text, g_v7.timer) != 0) {
            snprintf(g_v7.timer, sizeof(g_v7.timer), "%s", text);
            v7_text("speedRunTimer", text);
        }
    }
}

static void rounds_tick(const Client *client, const int on, const int relayout) {
    const GameMode *mode = client->game.mode;
    const int shown = on && mode && mode->game_rounds > 1 && !spectate_active();
    if (shown != g_v7.rounds_shown || relayout) {
        g_v7.rounds_shown = shown;
        v7_display("roundsDisplay", shown ? "inline-block" : "none");
    }
    if (!shown) return;
    const HudModesV7 *v7 = hud_modes_v7();
    const int left = v7->rounds_left >= 0 ? v7->rounds_left : mode->round_count;
    char text[32];
    snprintf(text, sizeof(text), "%d/%d", mode->game_rounds - (left > 0 ? left : 0), mode->game_rounds);
    if (strcmp(text, g_v7.rounds_val) != 0) {
        snprintf(g_v7.rounds_val, sizeof(g_v7.rounds_val), "%s", text);
        v7_text("roundsVal", text);
    }
    for (int t = 1; t <= 2; t++) {
        const int wins = v7->team_round_wins[t];
        if (wins == g_v7.round_wins[t - 1]) continue;
        g_v7.round_wins[t - 1] = wins;
        char id[16];
        snprintf(id, sizeof(id), "roundTSc%d", t);
        snprintf(text, sizeof(text), "%d", wins);
        v7_text(id, text);
    }
}

static void tags_tick(const Client *client) {
    const GameMode *mode = client->game.mode;
    const int limit = mode ? mode->config.tag_hold_limit : 0;
    const int tags = hud_modes_v7()->dog_tags;
    // "krt" (io_updateKrTags) writes the holder for every mode; the limit only turns the item red. CTF has
    // no tagHoldLimit, and its tags still show (official "krt" from a CTF server).
    if (!g_v7.tags_shown && tags <= 0) return;
    const int full = limit > 0 && tags >= limit;
    if (g_v7.tags_shown && tags == g_v7.tags && full == g_v7.tags_full) return;
    g_v7.tags = tags;
    g_v7.tags_full = full;
    const HtmlElem holder = html_dom_by_id("krTagHolder");
    if (!holder) return;
    const char *color = full ? "rgba(255, 0, 0, 0.5)" : "var(--div-grey)";
    char html[512];
    snprintf(html, sizeof(html), "<div class='killStreakItem'><div class='killStreakItemInner' style=\"background-color:%s\">"
                                 "<div class='killStreakCnt'>x%d</div><img style='width: 100%%;' src=\"textures/sprays/114.png\"></div></div>", color, tags);
    html_dom_set_html(holder, html);
    g_v7.tags_shown = 1;
}

static void bighead_tick(const Client *client, const int on, const int relayout) {
    const GameMode *mode = client->game.mode;
    const int shown = on && mode && mode->config.bighead;
    if (shown != g_v7.bighead_shown || relayout) {
        g_v7.bighead_shown = shown;
        v7_display("bhHolder", shown ? "inline-block" : "none");
    }
    if (!shown || !client->me) return;
    const float mlt = mode_head_mlt(client->me);
    if (mlt == g_v7.head_mlt) return;
    g_v7.head_mlt = mlt;
    const HtmlElem holder = html_dom_by_id("bhHolder");
    const HtmlElem head = holder ? html_dom_first_child(holder) : 0;
    if (!head) return;
    char size[48];
    snprintf(size, sizeof(size), "calc(40px * %g)", mlt);
    html_dom_set_style(head, "width", size);
    html_dom_set_style(head, "height", size);
}

// Per frame from client_hud_html_tick: `on` = the in-game HUD is up for a live player,
// `layout_epoch` bumps whenever hud_ui.c re-applied its visibility defaults.
void hud_v7modes_tick(Client *client, const int on, const int layout_epoch) {
    const int relayout = layout_epoch != g_v7.layout_epoch;
    g_v7.layout_epoch = layout_epoch;
    speed_run_tick(client, on, relayout);
    rounds_tick(client, on, relayout);
    tags_tick(client);
    bighead_tick(client, on, relayout);
}
