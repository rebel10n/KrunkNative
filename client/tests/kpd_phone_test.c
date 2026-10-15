// kpd_phone_test: the K.P.D phone (client/src/ui/hud_votekick.c) under a call with no answer, and
// every other way out of a call. hud_votekick.c runs for real; the page, the audio, the chat, the
// socket and the timers are stubs here, and the timers run on a fake clock this file advances.
//
//   * callPolice raises the phone (#phoneImg margin-bottom 0px), sends one "kpd" request and arms the
//     no-answer timer at KPD_NO_ANSWER_MS (15 s); nothing answers, so at 15 s the phone is down, the
//     busy tone plays and the K.P.D says it did not answer. A second call can go out after that.
//   * an answer (reported, refused, busy) takes the phone down at once and cancels the timer, so the
//     timer cannot end a later call. A late answer after the timeout is shown and the phone stays down.
//   * a new round (INIT: hud_votekick_reset) and a lost session (hud_votekick_on_session_lost) end a
//     call and cancel its timer; a second callPolice while a call runs sends nothing.
//   * with no socket (and not offline) nothing is sent: the phone is never raised.
// The officer's side runs here too (policeRes codes 1..3 and the verdict panel):
//   * a list in pages (name "page/pages") is joined and rendered into #kpdCalls with the last page;
//     a list for another tab than the one on show is ignored; v10's call order and rating colours;
//     the Stats tab's Range / Tag Count table
//   * kpdSubmit sends one "kpdA"; its answer (code 6) shows the success or the error popup
//   * [Y] twice (kpdVoteY) tags the case: one "rkpd" 1; no case or not spectating: the key does nothing
//   * a list request nobody answers (an official krunker.io server that sends an officer's list only to
//     its own officers, a dropped brain request) says "Request Failed" after KPD_NO_ANSWER_MS instead of
//     "Loading..." for good; an answer, another tab or a new request cancels that
//   * the case's suspect / caller (hud_votekick_kpd_sus / _caller: KPD Vision, the spectator cards)
//     belong to the connection the case came on
#include <ui_logic.h>
#include <client.h>
#include <html_dom.h>
#include <html_ui.h>
#include <audio.h>
#include <backend_ui.h>
#include <game_config.h>
#include <net_ext.h>
#include <spectate.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

// ------------------------------------------------------------------ fake clock and timers
#define FAKE_TIMERS 8
static struct { int id; double due; void (*fn)(void *); void *user; int live; } g_timers[FAKE_TIMERS];
static int g_timer_next = 1;
static double g_now_ms;
static double g_last_timer_ms;

int html_timer_add(const double ms, void (*fn)(void *user), void *user) {
    for (int i = 0; i < FAKE_TIMERS; i++) {
        if (g_timers[i].live) continue;
        g_timers[i].id = g_timer_next++;
        g_timers[i].due = g_now_ms + ms;
        g_timers[i].fn = fn;
        g_timers[i].user = user;
        g_timers[i].live = 1;
        g_last_timer_ms = ms;
        return g_timers[i].id;
    }
    return 0;
}

void html_timer_cancel(const int id) {
    if (id <= 0) return;
    for (int i = 0; i < FAKE_TIMERS; i++) {
        if (g_timers[i].live && g_timers[i].id == id) g_timers[i].live = 0;
    }
}

static int live_timers(void) {
    int n = 0;
    for (int i = 0; i < FAKE_TIMERS; i++) n += g_timers[i].live;
    return n;
}

// Run the page's clock forward: timers fire in due order, as html_timers_tick does
static void advance(const double ms) {
    const double end = g_now_ms + ms;
    for (;;) {
        int best = -1;
        for (int i = 0; i < FAKE_TIMERS; i++) {
            if (g_timers[i].live && g_timers[i].due <= end && (best < 0 || g_timers[i].due < g_timers[best].due)) best = i;
        }
        if (best < 0) break;
        g_now_ms = g_timers[best].due;
        g_timers[best].live = 0;
        g_timers[best].fn(g_timers[best].user);
    }
    g_now_ms = end;
}

// ------------------------------------------------------------------ page (DOM) stubs
#define ELEM_PHONE 1
#define ELEM_VOTE 2
#define ELEM_KPD_CALLS 3
static char *g_kpd_calls_html; // #kpdCalls' innerHTML (NULL: never set)
static int g_kpd_tab_shown;    // ui_loadout_kpd_tab()
static char g_phone_margin[32] = "-700px"; // the stylesheet's #phoneImg default is hidden (the INIT state)

HtmlElem html_dom_by_id(const char *id) {
    if (!strcmp(id, "phoneImg")) return ELEM_PHONE;
    if (!strcmp(id, "voteToKick")) return ELEM_VOTE;
    if (!strcmp(id, "kpdCalls")) return ELEM_KPD_CALLS;
    return 0;
}
HtmlElem html_dom_root(void) { return 0; }
int html_dom_query_all(HtmlElem root, const char *selector, HtmlElem *out, int max) { (void) root; (void) selector; (void) out; (void) max; return 0; }
void html_dom_set_html(HtmlElem e, const char *html) {
    if (e != ELEM_KPD_CALLS) return;
    free(g_kpd_calls_html);
    g_kpd_calls_html = strdup(html ? html : "");
}
void html_dom_set_attr(HtmlElem e, const char *name, const char *value) { (void) e; (void) name; (void) value; }
void html_dom_set_display(HtmlElem e, const char *value) { (void) e; (void) value; }
void html_dom_set_style(HtmlElem e, const char *prop, const char *value) {
    if (e == ELEM_PHONE && !strcmp(prop, "margin-bottom")) snprintf(g_phone_margin, sizeof(g_phone_margin), "%s", value);
}
void html_clipboard_write(const char *text) { (void) text; }

static int phone_up(void) { return !strcmp(g_phone_margin, "0px"); }

// ------------------------------------------------------------------ chat, audio, popups
#define LOG_MAX 32
static char g_chat[LOG_MAX][96];
static int g_chat_n;
static char g_sounds[LOG_MAX][24];
static int g_sounds_n;
static int g_popups_cleared;

void hud_ui_add_chat(const char *name, const char *msg, const char *color) {
    (void) name; (void) color;
    if (g_chat_n < LOG_MAX) snprintf(g_chat[g_chat_n++], sizeof(g_chat[0]), "%s", msg ? msg : "");
}
static int chat_has(const char *needle) {
    for (int i = 0; i < g_chat_n; i++) if (strstr(g_chat[i], needle)) return 1;
    return 0;
}
static int sounds_of(const char *name) {
    int n = 0;
    for (int i = 0; i < g_sounds_n; i++) n += !strcmp(g_sounds[i], name);
    return n;
}
void audio_play(const char *name, float volume, int loop, float rate) {
    (void) volume; (void) loop; (void) rate;
    if (g_sounds_n < LOG_MAX) snprintf(g_sounds[g_sounds_n++], sizeof(g_sounds[0]), "%s", name);
}
void audio_stop(const char *name) { (void) name; }
void ui_loadout_clear_pops(void) { g_popups_cleared++; }

// ------------------------------------------------------------------ game / session stubs
const AccountInfo *account_get(void) { return NULL; }
int game_config_is_custom(const Game *g) { (void) g; return 0; }
int servers_ui_join_game(const char *game_id) { (void) game_id; return 0; }
static int g_spectating;
int spectate_active(void) { return g_spectating; }
int spectate_focused_uid(void) { return -1; }
void spectate_focus_player(Client *client, int uid, int quiet_auto) { (void) client; (void) uid; (void) quiet_auto; }
static MenuUi g_menu; // the player list is not open (refresh_player_list)
MenuUi *menu_ui(void) { return &g_menu; }
void menu_show_window(int id, int force) { (void) id; (void) force; }
int ui_loadout_kpd_tab(void) { return g_kpd_tab_shown; }
void ui_loadout_switch_kpd_tab(int tab) { g_kpd_tab_shown = tab; }
static int g_apply_results, g_apply_ok;
static char g_apply_text[96];
void ui_loadout_kpd_apply_result(int ok, const char *text) {
    g_apply_results++;
    g_apply_ok = ok;
    snprintf(g_apply_text, sizeof(g_apply_text), "%s", text ? text : "");
}

// The packets the client sends: the count, and the last one's subtype
static int g_sent, g_last_sub, g_last_byte;
void client_net_send(Client *client, NetPacketType type, const void *payload, uint16_t length) {
    (void) client;
    uint16_t sub = 0, body_len = 0;
    const uint8_t *body = NULL;
    if (type == NET_PACKET_EXT && net_ext_unpack(payload, length, &sub, &body, &body_len)) {
        g_last_sub = sub;
        g_last_byte = body_len ? body[0] : -1;
    }
    g_sent++;
}

// ------------------------------------------------------------------ helpers
static Client *make_client(const int offline) {
    Client *c = calloc(1, sizeof(*c));
    pthread_mutex_init(&c->net_lock, NULL);
    c->offline = offline;
    c->net_socket = offline ? -1 : 7; // a live socket unless the test says otherwise
    return c;
}

static void call(Client *client, const char *alias, const int uid) {
    HtmlArg args[2];
    memset(args, 0, sizeof(args));
    args[0].kind = HTML_ARG_STR;
    args[0].str = alias;
    args[1].kind = HTML_ARG_NUM;
    args[1].num = uid;
    CHECK(hud_votekick_handle(client, "callPolice", NULL, args, 2) == 1, "callPolice is handled");
}

static void answer(Client *client, const uint8_t code, const uint8_t status, const char *message, const char *name) {
    NetKpdResponsePacket r;
    memset(&r, 0, sizeof(r));
    r.code = code;
    r.status = status;
    snprintf(r.message, sizeof(r.message), "%s", message);
    snprintf(r.name, sizeof(r.name), "%s", name);
    hud_votekick_on_kpd(client, &r, NULL, 0);
}

static void reset_world(void) {
    memset(g_timers, 0, sizeof(g_timers));
    g_now_ms = 0;
    g_last_timer_ms = 0;
    snprintf(g_phone_margin, sizeof(g_phone_margin), "%s", "-700px");
    g_chat_n = 0;
    g_sounds_n = 0;
    g_popups_cleared = 0;
    g_sent = 0;
    g_last_sub = 0;
    hud_votekick_reset(); // the INIT state of a fresh session: nothing in flight, phone down
    g_sent = 0; // the reset itself sends nothing; keep the counters at zero for the case
    g_chat_n = 0;
    g_sounds_n = 0;
}

// ------------------------------------------------------------------ cases
static void case_no_answer_times_out(void) {
    reset_world();
    Client *c = make_client(1);
    call(c, "carol", 9);
    CHECK(phone_up(), "the call raises the phone");
    CHECK(g_sent == 1 && g_last_sub == NET_EXT_KPD_CALL, "one kpd request goes out (sent %d)", g_sent);
    CHECK(g_last_timer_ms == 15000.0, "the no-answer timer is 15 s (got %.0f ms)", g_last_timer_ms);
    advance(14999);
    CHECK(phone_up(), "the phone is still up just before the timeout");
    advance(1);
    CHECK(!phone_up(), "the phone is hidden once the timeout passes with no answer");
    CHECK(chat_has("did not answer"), "the K.P.D says it did not answer");
    CHECK(sounds_of("busy_0") == 1, "the busy tone plays once (%d)", sounds_of("busy_0"));
    CHECK(live_timers() == 0, "no timer left armed");
    // the call is over: a new one goes out and raises the phone again
    call(c, "carol", 9);
    CHECK(phone_up() && g_sent == 2, "after the timeout a new call raises the phone and sends (sent %d)", g_sent);
    free(c);
}

static void case_reported_answer_cancels_timer(void) {
    reset_world();
    Client *c = make_client(1);
    call(c, "carol", 9);
    answer(c, 0, 0, "Reported carol to the K.P.D", "carol"); // NET_KPD_REPORTED
    CHECK(!phone_up(), "a report takes the phone down at once");
    CHECK(chat_has("Reported carol"), "the report is said in chat");
    CHECK(live_timers() == 0, "the report cancels the no-answer timer");
    advance(60000);
    CHECK(!chat_has("did not answer") && sounds_of("busy_0") == 0, "the cancelled timer does not fire later");
    free(c);
}

static void case_refused_answer(void) {
    reset_world();
    Client *c = make_client(1);
    call(c, "dave", 3);
    answer(c, 0, 2, "Requires Lvl 5", ""); // NET_KPD_REFUSED
    CHECK(!phone_up(), "a refusal takes the phone down at once");
    CHECK(chat_has("Requires Lvl 5"), "the refusal's message is said in chat");
    CHECK(sounds_of("busy_0") == 1, "a refusal plays the busy tone");
    free(c);
}

static void case_second_call_during_call(void) {
    reset_world();
    Client *c = make_client(1);
    call(c, "carol", 9);
    call(c, "carol", 9);
    CHECK(g_sent == 1, "a second call during a call sends nothing (sent %d)", g_sent);
    CHECK(phone_up(), "the phone stays up for the call in flight");
    CHECK(live_timers() == 1, "one timer for the one call");
    advance(15000);
    CHECK(!phone_up(), "the one timer still takes the phone down");
    free(c);
}

static void case_new_round_ends_call(void) {
    reset_world();
    Client *c = make_client(1);
    call(c, "carol", 9);
    hud_votekick_reset(); // INIT: a new round
    CHECK(!phone_up(), "a new round takes the phone down");
    CHECK(live_timers() == 0, "a new round cancels the timer");
    advance(60000);
    CHECK(!chat_has("did not answer"), "nothing fires into the new round");
    call(c, "carol", 9);
    CHECK(phone_up() && g_sent == 2, "a call in the new round goes out (sent %d)", g_sent);
    free(c);
}

static void case_session_lost_ends_call(void) {
    reset_world();
    Client *c = make_client(1);
    call(c, "carol", 9);
    hud_votekick_on_session_lost();
    CHECK(!phone_up(), "a lost session takes the phone down at once");
    CHECK(live_timers() == 0, "a lost session cancels the timer");
    advance(60000);
    CHECK(!chat_has("did not answer"), "nothing fires after the session is lost");
    free(c);
}

static void case_no_socket_raises_nothing(void) {
    reset_world();
    Client *c = make_client(0);
    c->net_socket = -1; // the socket is down: client_net_send would drop the request
    call(c, "carol", 9);
    CHECK(!phone_up(), "with no socket the phone is not raised");
    CHECK(g_sent == 0, "with no socket nothing is sent");
    CHECK(chat_has("cannot be called"), "the player is told the K.P.D cannot be called");
    CHECK(live_timers() == 0, "no timer for a call that never left");
    free(c);
}

static void case_late_answer_after_timeout(void) {
    reset_world();
    Client *c = make_client(1);
    call(c, "carol", 9);
    advance(15000);
    CHECK(!phone_up(), "timed out");
    answer(c, 0, 0, "Reported carol to the K.P.D", "carol");
    CHECK(!phone_up(), "a late report leaves the phone down");
    CHECK(chat_has("Reported carol"), "a late report is still said");
    free(c);
}

static void case_scenario_phone(void) {
    reset_world();
    hud_votekick_scenario_phone(1);
    CHECK(phone_up(), "the capture hook raises the phone");
    hud_votekick_scenario_phone(0);
    CHECK(!phone_up() && live_timers() == 0, "the capture hook drops it again");
}

static void list_page(Client *client, const uint8_t code, const char *page, const char *json) {
    NetKpdResponsePacket r;
    memset(&r, 0, sizeof(r));
    r.code = code;
    r.status = NET_KPD_REPORTED;
    snprintf(r.name, sizeof(r.name), "%s", page);
    hud_votekick_on_kpd(client, &r, json, strlen(json));
}

static void case_officer_list_pages(void) {
    reset_world();
    Client *c = make_client(0);
    free(g_kpd_calls_html);
    g_kpd_calls_html = NULL;
    g_kpd_tab_shown = 0; // Active Calls
    list_page(c, 1, "1/2", "[{\"can\":\"rep1\",\"con\":\"susA\",\"gst\":0,\"gco\":\"NODE:a1\",\"typ\":0,\"jop\":0,\"jor\":3}]");
    CHECK(!g_kpd_calls_html, "page 1 of 2 renders nothing yet");
    list_page(c, 1, "2/2", "[{\"can\":\"rep2\",\"con\":\"susB\",\"gst\":0,\"gco\":\"NODE:b2\",\"typ\":1,\"jop\":5,\"jor\":5}]");
    const char *h = g_kpd_calls_html ? g_kpd_calls_html : "";
    CHECK(strstr(h, "susA") && strstr(h, "susB") && strstr(h, "policeJoin('NODE:a1')") && strstr(h, "policeJoin('NODE:b2')"),
          "the last page renders both pages' calls with their Join: %s", h);
    CHECK(strstr(h, "susB") < strstr(h, "susA"), "v10's order: the ranked call first");
    CHECK(strstr(h, "[ranked] rep2") && strstr(h, "color:#fc3232'>0/3") && strstr(h, "color:#63de26'>5/5"),
          "v10's type tag and rating colours: %s", h);
    CHECK(strstr(h, "<span class='callRegion'>NODE</span>"), "the Info column's server (gco before ':')");
    list_page(c, 2, "", "[{\"modn\":\"m\",\"htag\":1,\"hckn\":\"x\"}]");
    CHECK(g_kpd_calls_html && !strstr(g_kpd_calls_html, "Moderator"), "a Ban Logs answer while Active Calls shows is ignored");
    list_page(c, 1, "", "[]");
    CHECK(g_kpd_calls_html && !strcmp(g_kpd_calls_html, "No Active Distress Calls"), "no calls: %s", g_kpd_calls_html);
    g_kpd_tab_shown = 2; // Stats
    list_page(c, 3, "", "[{\"1dayEntries\":2,\"alltimeEntries\":7}]");
    h = g_kpd_calls_html ? g_kpd_calls_html : "";
    CHECK(strstr(h, "Tag Count") && strstr(h, "1day</span></td><td><span style='color:#858585'>2<") &&
              strstr(h, "alltime</span></td><td><span style='color:#858585'>7<"), "the Stats table: %s", h);
    g_kpd_tab_shown = 0;
    g_sent = 0;
    hud_votekick_kpd_request(c, 1);
    CHECK(g_sent == 1 && g_last_sub == NET_EXT_KPD_CALL && g_last_byte == 1, "switchKPDTab(0): one \"kpd\" tab 1 request");
    c->net_socket = -1;
    hud_votekick_kpd_request(c, 1);
    CHECK(g_sent == 1 && g_kpd_calls_html && !strcmp(g_kpd_calls_html, "Not connected to a server"),
          "no session: nothing sent, the list says why (%s)", g_kpd_calls_html);
    free(c);
}

static void vote(Client *client, const int yes) { hud_votekick_kpd_key(client, yes); } // kpdVoteY / kpdVoteN

static void case_verdict_keys(void) {
    reset_world();
    Client *c = make_client(0);
    c->net_client_id = 4;
    g_spectating = 1;
    vote(c, 1);
    CHECK(g_sent == 0, "no case: Y sends nothing");
    const NetKpdDataPacket d = {.sus = 9, .caller = 3};
    hud_votekick_on_kpd_data(c, &d);
    g_spectating = 0;
    vote(c, 1);
    CHECK(g_sent == 0, "not spectating: Y sends nothing");
    g_spectating = 1;
    vote(c, 1); // "Are you 100% sure?"
    CHECK(g_sent == 0, "the first YES asks again");
    vote(c, 1); // tagged
    CHECK(g_sent == 1 && g_last_sub == NET_EXT_KPD_RATE && g_last_byte == 1, "the second YES sends rkpd 1 (%d %#x %d)", g_sent, g_last_sub, g_last_byte);
    vote(c, 0);
    CHECK(g_sent == 1, "the case is submitted: no second verdict");
    hud_votekick_on_kpd_reset(c); // "kpdf": watch longer, the verdict is open again
    vote(c, 0);
    CHECK(g_sent == 2 && g_last_sub == NET_EXT_KPD_RATE && g_last_byte == 0, "after kpdf, NO sends rkpd 0");
    g_spectating = 0;
    free(c);
}

// kpdSubmit's "kpdA" and its answer (policeRes code 6)
static void case_application(void) {
    reset_world();
    Client *c = make_client(0);
    g_apply_results = 0;
    hud_votekick_kpd_apply(c, "cop#1", "I hunt hackers");
    CHECK(g_sent == 1 && g_last_sub == NET_EXT_KPD_APPLY && g_last_byte == 'c', "kpdSubmit sends one kpdA (%d %#x)", g_sent, g_last_sub);
    answer(c, 6, NET_KPD_REPORTED, "Application sent! The K.P.D will review it.", "");
    CHECK(g_apply_results == 1 && g_apply_ok && strstr(g_apply_text, "Application sent"), "received: the success popup (%s)", g_apply_text);
    answer(c, 6, NET_KPD_REFUSED, "Your application is already in review", "");
    CHECK(g_apply_results == 2 && !g_apply_ok && strstr(g_apply_text, "already in review"), "refused: the error popup (%s)", g_apply_text);
    CHECK(!phone_up() && !chat_has("review"), "an application's answer is no call's: no phone, no chat");
    c->net_socket = -1;
    g_sent = 0;
    hud_votekick_kpd_apply(c, "cop#1", "again");
    CHECK(g_sent == 0 && g_apply_results == 3 && !g_apply_ok, "no session: nothing sent, the error popup");
    free(c);
}

static void case_list_no_answer(void) {
    reset_world();
    Client *c = make_client(0);
    free(g_kpd_calls_html);
    g_kpd_calls_html = strdup("Loading...");
    g_kpd_tab_shown = 0;
    hud_votekick_kpd_request(c, 1);
    CHECK(g_sent == 1 && live_timers() == 1 && g_last_timer_ms == 15000.0, "the request arms a 15 s no-answer timer (%d timers)", live_timers());
    advance(14999);
    CHECK(!strcmp(g_kpd_calls_html, "Loading..."), "still loading just before the timeout");
    advance(1);
    CHECK(!strcmp(g_kpd_calls_html, "Request Failed"), "no answer: v10's failed-request text, not Loading... for good (%s)", g_kpd_calls_html);
    CHECK(live_timers() == 0, "no timer left");

    // an answer in time cancels it (a page of two counts: the rest is on its way)
    free(g_kpd_calls_html);
    g_kpd_calls_html = strdup("Loading...");
    hud_votekick_kpd_request(c, 1);
    list_page(c, 1, "1/2", "[{\"can\":\"rep1\",\"con\":\"susA\",\"typ\":0}]");
    CHECK(live_timers() == 0, "the first page cancels the timer");
    list_page(c, 1, "2/2", "[{\"can\":\"rep2\",\"con\":\"susB\",\"typ\":0}]");
    advance(60000);
    CHECK(strstr(g_kpd_calls_html, "susA") && strstr(g_kpd_calls_html, "susB"), "the answered list stays (%s)", g_kpd_calls_html);

    // the officer moved to another tab: the old request's timeout leaves the new tab alone
    hud_votekick_kpd_request(c, 1);
    g_kpd_tab_shown = 3; // Demos: no request, its own text
    free(g_kpd_calls_html);
    g_kpd_calls_html = strdup("K.P.D demos are not recorded on this server");
    advance(15000);
    CHECK(!strcmp(g_kpd_calls_html, "K.P.D demos are not recorded on this server"), "another tab's text is kept (%s)", g_kpd_calls_html);

    // a second request replaces the first one's timer (one timer, the newest tab)
    g_kpd_tab_shown = 1;
    hud_votekick_kpd_request(c, 1);
    hud_votekick_kpd_request(c, 2);
    CHECK(live_timers() == 1, "one timer for the newest request (%d)", live_timers());
    free(g_kpd_calls_html);
    g_kpd_calls_html = strdup("Loading...");
    list_page(c, 1, "", "[]"); // a late Active Calls answer: not the tab on show
    advance(15000);
    CHECK(!strcmp(g_kpd_calls_html, "Request Failed"), "Ban Logs never answered: Request Failed (%s)", g_kpd_calls_html);
    g_kpd_tab_shown = 0;
    free(c);
}

static void case_vision_case_owner(void) {
    reset_world();
    Client *c = make_client(0);
    c->net_client_id = 6; // a connection no case came on (case_verdict_keys' was on 4)
    CHECK(hud_votekick_kpd_sus(c) == -1 && hud_votekick_kpd_caller(c) == -1, "no case: no suspect, no caller");
    c->net_client_id = 4;
    const NetKpdDataPacket d = {.sus = 9, .caller = 3};
    hud_votekick_on_kpd_data(c, &d);
    CHECK(hud_votekick_kpd_sus(c) == 9 && hud_votekick_kpd_caller(c) == 3, "the case names suspect 9 and caller 3 (%d %d)",
          hud_votekick_kpd_sus(c), hud_votekick_kpd_caller(c));
    c->net_client_id = 5; // a new connection (another server): the case stays behind
    CHECK(hud_votekick_kpd_sus(c) == -1 && hud_votekick_kpd_caller(c) == -1, "a new connection has no case");
    c->net_client_id = 4;
    const NetKpdDataPacket none = {.sus = -1, .caller = -1};
    hud_votekick_on_kpd_data(c, &none);
    CHECK(hud_votekick_kpd_sus(c) == -1 && hud_votekick_kpd_caller(c) == -1, "kpdData null ends the case");
    free(c);
}

int main(void) {
    case_no_answer_times_out();
    case_reported_answer_cancels_timer();
    case_refused_answer();
    case_second_call_during_call();
    case_new_round_ends_call();
    case_session_lost_ends_call();
    case_no_socket_raises_nothing();
    case_late_answer_after_timeout();
    case_scenario_phone();
    case_officer_list_pages();
    case_verdict_keys();
    case_application();
    case_list_no_answer();
    case_vision_case_owner();
    free(g_kpd_calls_html);
    printf("kpd_phone_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
