// The player list's actions (Krunker v10.0.2 game.deob.js, windows[22] genList :214300): the
// Votekick button and the vote prompt (#voteToKick: kickVote :220193, io "vk" :219974, the
// kickVoteY / kickVoteN keys :221373), the K.P.D button with its confirm popup and call
// (confirmPolice / callPolice :216268-216274, policeRes code 0 :216287) and copyUser. The
// packets are NET_PACKET_EXT subtypes (shared/include/net_ext.h 0x0800..); the rules are the
// server's (server/src/votekick.c).
#include <ui_logic.h>
#include <player_names.h> // Hide player names (player_names.h)
#include <client.h>
#include <html_dom.h>
#include <html_ui.h>
#include <hud_social.h>
#include <player_accounts.h>
#include <backend_ui.h> // active_account: v10 shows Votekick / K.P.D to accounts only; servers_ui_join_game (policeJoin)
#include <audio.h>
#include <net_ext.h>
#include <game_config.h> // game_config_is_custom: v10 isCustom
#include <spectate.h> // the officer's suspect: focusPlayer(sus, false, true)
#include <pthread.h> // client->net_lock: the socket check before a call goes out
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h> // the Distress Calls' "N ago"
#include <math.h> // kpd_level: v10's Math.sqrt

#define VOTED_MAX 32

// K.P.D call with no answer: the phone comes down and the busy tone plays after this long. The
// answer normally takes well under a second (the server answers at once; a call the brain records
// waits one kn_brain round trip, accounts.c accounts_kpd_request). A reply that never comes (a
// dropped brain request or socket, a packet the link cannot address) would otherwise leave the
// phone up and the K.P.D button dead until the round ends. 15 s is about fifteen times the usual
// answer and far below a player's patience with a phone that stays up.
#define KPD_NO_ANSWER_MS 15000.0

static struct {
    // io "vk": the running vote (voteToKick), who it targets and how it stands
    int active;
    char target[32];
    char initiator[32];
    int votes, needed;
    int initiated;             // iïíiiíî: this player opened a vote this session (no second one)
    char voted[VOTED_MAX][32]; // iiíiiìî: aliases this player already voted on
    int voted_count;
    // K.P.D (callPolice): one call in flight (iiììííi), the phone up
    int calling;
    char suspect[32];
    int suspect_uid;
    int call_timer;            // the no-answer timer (html_timer_add), 0 none
    int phone_up;              // #phoneImg margin-bottom 0px (v10 the phone up)
    int phone_reported;        // a report went out: the picture blinks until the drop (v10 _71755)
    int blink_on;              // the blink's picture: phone_r (1) or phone_b (0)
    int ring_timer, blink_timer, drop_timer; // v10 _71757 (the ring), _71756 (the blink), the 2200 ms drop; 0 none
    // K.P.D officer (v10 kpdData {sus, caller}, NET_EXT_KPD_DATA): the case of this connection
    int kpd_case;              // a case (iííiìíî != null)
    int kpd_conn;              // client->net_client_id it came on (a new connection: no case)
    int kpd_sus, kpd_caller;   // the suspect's / reporter's uid (-1 none)
    int kpd_step;              // iìíîiíi: -1 not tagged, 0 "Is X hacking?", 1 "Are you 100% sure?", 2 tagged
    int kpd_done;              // iîíìiïi: the verdict went out ("rkpd")
    // an officer's list arriving in pages (policeRes code list_code), joined until the last one
    cJSON *list_rows;
    int list_code;
    int list_timer;            // the list request's no-answer timer, 0 none
    int list_wait;             // the code it waits for (tab + 1)
    // --render-scenario / tests: the rows come from here instead of the live game
    cJSON *override;
} g_vk;
static int g_scenario_lists; // --render-scenario state.kpdPolice: the lists come from the fixture, no request

static void dom_display(const char *id, const char *value) {
    const HtmlElem el = html_dom_by_id(id);
    if (el) html_dom_set_display(el, value);
}

static void dom_html(const char *id, const char *html) {
    const HtmlElem el = html_dom_by_id(id);
    if (el) html_dom_set_html(el, html);
}

static void append_esc(char *out, const size_t size, size_t *len, const char *text) {
    for (const char *p = text; *p && *len + 8 < size; p++) {
        const char *rep = *p == '<' ? "&lt;" : *p == '>' ? "&gt;" : *p == '&' ? "&amp;" : *p == '"' ? "&quot;" : *p == '\'' ? "&#39;" : NULL;
        if (rep) {
            const size_t n = strlen(rep);
            memcpy(out + *len, rep, n);
            *len += n;
        } else out[(*len)++] = *p;
    }
    out[*len] = 0;
}

static int voted_on(const char *alias) {
    for (int i = 0; i < g_vk.voted_count; i++) {
        if (!strcmp(g_vk.voted[i], alias)) return 1;
    }
    return 0;
}

static void remember_vote(const char *alias) {
    if (voted_on(alias) || g_vk.voted_count >= VOTED_MAX) return;
    snprintf(g_vk.voted[g_vk.voted_count++], sizeof(g_vk.voted[0]), "%s", alias);
}

static const char *my_alias(const Client *client) {
    if (!client) return "";
    if (client->me && client->me->name[0]) return client->me->name;
    return client->menu_name;
}

static void send_ext(Client *client, const uint16_t subtype, const void *body, const uint16_t length) {
    uint8_t buf[NET_EXT_HEADER_SIZE + NET_EXT_BODY_MAX];
    const uint16_t n = net_ext_pack(buf, sizeof(buf), subtype, body, length);
    if (n && client) client_net_send(client, (NetPacketType) NET_PACKET_EXT, buf, n);
}

static void send_vote(Client *client, const char *target, const int yes) {
    NetVoteKickPacket req;
    memset(&req, 0, sizeof(req));
    snprintf(req.target, sizeof(req.target), "%s", target);
    req.vote = (uint8_t) (yes ? 1 : 0);
    send_ext(client, NET_EXT_VOTE_KICK, &req, sizeof(req));
}

static void refresh_player_list(void) {
    if (menu_ui()->active_window == 23) menu_show_window(23, 1); // updateWindow(23)
}

// ---------------------------------------------------------------- the rows (hud_ui.c)

void hud_votekick_set_override(const cJSON *rows) {
    cJSON_Delete(g_vk.override);
    g_vk.override = rows ? cJSON_Duplicate(rows, 1) : NULL;
}

const cJSON *hud_votekick_override(void) { return g_vk.override; }

// v10 :214300: `account && !isCustom && !voted(alias) && (voteTarget == alias || (!voteTarget &&
// !initiated)) && !isRanked`. isCustom: a hosted lobby (its host kicks); the sandbox has no host.
int hud_votekick_row_allowed(const Client *client, const char *alias, const int level_ok) {
    (void) level_ok;
    if (!client || !active_account() || !alias || !alias[0]) return 0;
    if (client->game.is_ranked) return 0;
    // !game.isCustom: the offline sandbox is hosted as custom on the server, but v10's sandbox is not,
    // and the client never receives the server's is_sandbox flag, so client->offline stands in for it
    if (game_config_is_custom(&client->game) && !client->game.is_sandbox && !client->offline) return 0;
    if (!strcmp(alias, my_alias(client))) return 0;
    if (voted_on(alias)) return 0;
    if (g_vk.active) return !strcmp(g_vk.target, alias);
    return !g_vk.initiated;
}

// v10 :214300 iîÌîìiï: `account && account.level >= 5`
int hud_votekick_kpd_row_allowed(const Client *client) {
    const AccountInfo *acc = active_account();
    return client && acc && acc->level >= 5;
}

// ---------------------------------------------------------------- the prompt (io "vk")

// The name a vote popup shows for an alias (Hide player names); the vote itself sends the alias
static const char *vk_shown(const Client *client, const char *alias, char *out, const size_t size) {
    for (size_t i = 0; client && i < client->game.player_count; i++) {
        const Player *p = client->game.players[i];
        if (p && !strcmp(p->name, alias)) return player_names_shown(p->uid, p == client->me, alias, out, size);
    }
    return player_names_shown(-1, 0, alias, out, size);
}

static void show_prompt(const Client *client, const int opened) {
    char shown[PLAYER_NAME_SIZE];
    char html[160];
    size_t len = 0;
    html[0] = 0;
    if (opened) { // "<voter> voted to kick <target>"
        append_esc(html, sizeof(html), &len, vk_shown(client, g_vk.initiator, shown, sizeof(shown)));
        append_esc(html, sizeof(html), &len, " voted to kick ");
        append_esc(html, sizeof(html), &len, vk_shown(client, g_vk.target, shown, sizeof(shown)));
        dom_html("voteKickInfo", html);
        len = 0;
        html[0] = 0;
        append_esc(html, sizeof(html), &len, "Kick ");
        append_esc(html, sizeof(html), &len, vk_shown(client, g_vk.target, shown, sizeof(shown)));
        dom_html("voteKickName", html);
    }
    snprintf(html, sizeof(html), "Votes: %d/%d", g_vk.votes, g_vk.needed);
    dom_html("voteKickNum", html);
    dom_display("voteToKick", "block");
    // the initiator sees no name / keys row (its vote is in); neither does whoever voted already
    const int mine = !strcmp(g_vk.initiator, my_alias(client)) || voted_on(g_vk.target);
    dom_display("voteKickName", mine ? "none" : "block");
    dom_display("voteKickKeys", mine ? "none" : "block");
}

void hud_votekick_on_state(Client *client, const NetVoteKickStatePacket *state) {
    if (!state) return;
    if (!state->active) { // "vk" null: the vote ended
        g_vk.active = 0;
        g_vk.target[0] = 0;
        dom_display("voteToKick", "none");
        refresh_player_list();
        return;
    }
    char target[32], voter[32];
    memcpy(target, state->target, sizeof(target));
    memcpy(voter, state->voter, sizeof(voter));
    target[sizeof(target) - 1] = voter[sizeof(voter) - 1] = 0;
    g_vk.votes = state->votes;
    g_vk.needed = state->needed;
    if (state->opened || !g_vk.active) {
        g_vk.active = 1;
        snprintf(g_vk.target, sizeof(g_vk.target), "%s", target);
        snprintf(g_vk.initiator, sizeof(g_vk.initiator), "%s", voter[0] ? voter : g_vk.initiator);
        if (!strcmp(g_vk.initiator, my_alias(client))) { // iïÌííìì == me.alias: initiated once per session
            g_vk.initiated = 1;
            remember_vote(target);
        }
        show_prompt(client, 1);
    } else {
        show_prompt(client, 0);
    }
    refresh_player_list();
}

// ---------------------------------------------------------------- forfeit vote

// io "vf" (v10 iíìiíìí, game.deob.js:220002; ranked): isNew -> "<initiator> initiated vote" + the count,
// updateOnly -> the count, a voter -> us: chat "Vote Submitted - a/b" (kind 2), anyone else: the box
// with the count; null -> hidden. The keys row stays "Enter /ff to vote" (the vote is a chat command).
void hud_votekick_on_forfeit(Client *client, const NetVoteForfeitStatePacket *state) {
    if (!state) return;
    if (!state->active) {
        dom_display("voteToForfeit", "none");
        return;
    }
    char voter[32], initiator[32], html[160];
    memcpy(voter, state->voter, sizeof(voter));
    memcpy(initiator, state->initiator, sizeof(initiator));
    voter[sizeof(voter) - 1] = initiator[sizeof(initiator) - 1] = 0;
    snprintf(html, sizeof(html), "Votes: %d/%d", state->votes, state->needed);
    if (!state->opened && !state->update_only && !strcmp(voter, my_alias(client))) {
        char line[64];
        snprintf(line, sizeof(line), "Vote Submitted - %d/%d", state->votes, state->needed);
        hud_ui_add_chat(NULL, line, "#fc03ec"); // addChat(-1, null, .., 2): a server line's default colour
        return;
    }
    if (state->opened) {
        char info[160];
        size_t len = 0;
        info[0] = 0;
        append_esc(info, sizeof(info), &len, initiator);
        append_esc(info, sizeof(info), &len, " initiated vote");
        dom_html("voteForfeitInfo", info);
    }
    dom_html("voteForfeitNum", html);
    dom_display("voteToForfeit", "block");
}

// kickVoteY / kickVoteN (v10 :221373): only while a vote runs, the player is alive, and they have
// not voted on this target yet. [1] sends v2k(target, 1), [2] v2k(target) and hides the prompt.
int hud_votekick_key(Client *client, const int yes) {
    if (!g_vk.active || !client || !client->me || !client->me->active || !active_account()) return 0;
    if (voted_on(g_vk.target)) return 0;
    send_vote(client, g_vk.target, yes);
    remember_vote(g_vk.target);
    if (!yes) dom_display("voteToKick", "none");
    else {
        dom_display("voteKickName", "none");
        dom_display("voteKickKeys", "none");
    }
    return 1;
}

// kickVote(id) (v10 :220193): v2k(alias, 1), the alias remembered, the list redrawn
static void kick_vote(Client *client, const int uid) {
    if (!client) return;
    const char *alias = NULL;
    for (size_t i = 0; i < client->game.player_count; i++) {
        const Player *p = client->game.players[i];
        if (p && p->uid == uid) {
            alias = p->name;
            break;
        }
    }
    if (!alias || !alias[0]) return;
    send_vote(client, alias, 1);
    remember_vote(alias);
    g_vk.initiated = 1;
    refresh_player_list();
}

// ---------------------------------------------------------------- K.P.D

// confirmPolice(alias, id): v10's genericPopup(..., "polConfPop")
static void confirm_police(const char *alias, const int uid) {
    if (g_vk.calling) return;
    char esc[96], shown_esc[96], shown[PLAYER_NAME_SIZE];
    size_t len = 0, shown_len = 0;
    esc[0] = 0;
    shown_esc[0] = 0;
    append_esc(esc, sizeof(esc), &len, alias); // callPolice takes the alias
    append_esc(shown_esc, sizeof(shown_esc), &shown_len, player_names_shown(uid, 0, alias, shown, sizeof(shown))); // the text shows the name as shown
    char html[900];
    snprintf(html, sizeof(html),
             "<div style='color:rgba(255,255,255,0.6);font-size:20px;margin-bottom:15px;'>Only call the <b>K.P.D</b> if you strongly suspect <b>%s</b>"
             " of using Hacks. <div style='height:10px'></div>False Reports can result in a Ban or Loss of Perms.</div>"
             "<div id='confirmBtn' style='width:142px;margin-right:5px' onclick='callPolice(`%s`,`%d`)'>Call<span class='material-icons'"
             " style='color:#fff;margin-left:10px;font-size:28px;vertical-align:bottom;margin-bottom:1px'>call</span></div>"
             "<div id='declineBtn' style='width:142px;margin-left:3px' onclick='clearPops()'>Cancel</div>",
             shown_esc, esc, uid);
    ui_loadout_clear_pops();
    dom_display("popupHolder", "block");
    const HtmlElem pop = html_dom_by_id("genericPop");
    if (!pop) return;
    html_dom_set_attr(pop, "class", "polConfPop");
    html_dom_set_attr(pop, "style", "");
    html_dom_set_html(pop, html);
    html_dom_set_display(pop, "block");
}

// The K.P.D phone (v10 callPolice :216044, policeRes 0 :216063, the frame loop :221448). It rises with
// the call (#phoneImg margin-bottom 0px; the stylesheet's 0.1 s ease-out slide). The ring plays 700 ms
// later and every 2200 ms while it is up without an answer. A report plays the siren and the picture
// blinks (phone_r / phone_b every 100..300 ms) until the phone drops 2200 ms after the answer, with the
// whoosh; a busy line drops it at once.
#define PHONE_RING_FIRST_MS 700
#define PHONE_RING_EVERY_MS 2200
#define PHONE_DROP_REPORTED_MS 2200
#define PHONE_BLINK_MIN_MS 100
#define PHONE_BLINK_MAX_MS 300

static void phone_ring(void *user);
static void phone_blink(void *user);
static void phone_drop(void *user);

// the ring, the blink and the drop are stopped (v10 _71755 false: a new call starts with none of them)
static void phone_timers_stop(void) {
    if (g_vk.ring_timer) html_timer_cancel(g_vk.ring_timer);
    if (g_vk.blink_timer) html_timer_cancel(g_vk.blink_timer);
    if (g_vk.drop_timer) html_timer_cancel(g_vk.drop_timer);
    g_vk.ring_timer = g_vk.blink_timer = g_vk.drop_timer = 0;
    g_vk.phone_reported = 0;
    g_vk.blink_on = 0;
}

// #phoneImg's margin-bottom: 0px up, -700px down
static void phone(const int up) {
    const HtmlElem img = html_dom_by_id("phoneImg");
    if (img) html_dom_set_style(img, "margin-bottom", up ? "0px" : "-700px");
    g_vk.phone_up = up;
}

static void phone_ring(void *user) {
    (void) user;
    g_vk.ring_timer = 0;
    if (!g_vk.phone_up || g_vk.phone_reported) return;
    audio_play("ring_0", 0.2f, 0, 1.0f);
    g_vk.ring_timer = html_timer_add(PHONE_RING_EVERY_MS, phone_ring, NULL);
}

static void phone_blink(void *user) {
    (void) user;
    g_vk.blink_timer = 0;
    if (!g_vk.phone_up || !g_vk.phone_reported) return;
    // v10 swaps ./img/phone_r.png and ./img/phone_b.png here. phone_b.png is not in the v10 mirror
    // (ui/img has phone_r.png only), so the picture stays phone_r until that asset is imported: the timing is v10's.
    g_vk.blink_on = !g_vk.blink_on;
    g_vk.blink_timer = html_timer_add(PHONE_BLINK_MIN_MS + rand() % (PHONE_BLINK_MAX_MS - PHONE_BLINK_MIN_MS + 1), phone_blink, NULL);
}

static void phone_drop(void *user) {
    (void) user;
    g_vk.drop_timer = 0;
    phone_timers_stop();
    phone(0);
    audio_play("whoosh_0", 0.1f, 0, 1.0f);
}

// The phone is down: nothing rings, blinks or drops after this
static void phone_down(void) {
    phone_timers_stop();
    phone(0);
}

// The call is over: the phone down, the no-answer timer cancelled (so it cannot end a later call)
// and no call in flight. Every way out of a call comes through here: the timeout, a new round (INIT)
// and a lost session. An answer goes through hud_votekick_on_kpd (a report keeps the phone up to drop).
static void call_end(void) {
    if (g_vk.call_timer) html_timer_cancel(g_vk.call_timer);
    g_vk.call_timer = 0;
    g_vk.calling = 0;
    phone_down();
}

// The no-answer timeout fired: nothing came back in KPD_NO_ANSWER_MS, so the call ends as busy.
static void call_no_answer(void *user) {
    (void) user;
    g_vk.call_timer = 0; // this timer has fired
    if (!g_vk.calling) return;
    call_end();
    audio_stop("ring_0");
    audio_play("busy_0", 0.1f, 0, 1.0f);
    hud_ui_add_chat(NULL, "The K.P.D did not answer", "#fc03ec");
    audio_play("whoosh_0", 0.1f, 0, 1.0f);
}

// A call goes out only on a live session: the socket (native, web or official), or the offline
// sandbox. client_net_send drops a packet without a socket, and then no answer can come.
static int call_reachable(Client *client) {
    if (client->offline) return 1;
    pthread_mutex_lock(&client->net_lock);
    const int socket = client->net_socket;
    pthread_mutex_unlock(&client->net_lock);
    return socket >= 0;
}

// callPolice(alias, id): clearPops, the dial / whoosh, "Calling K.P.D ...", "kpd" 0 {id}. The phone
// stays up until the answer or KPD_NO_ANSWER_MS, whichever comes first.
static void call_police(Client *client, const char *alias, const int uid) {
    if (!client) return;
    ui_loadout_clear_pops(); // the confirm popup closes whatever happens to the call
    if (g_vk.calling) return;
    if (!call_reachable(client)) {
        hud_ui_add_chat(NULL, "The K.P.D cannot be called while disconnected", "#fc03ec");
        return;
    }
    g_vk.calling = 1;
    g_vk.suspect_uid = uid;
    snprintf(g_vk.suspect, sizeof(g_vk.suspect), "%s", alias ? alias : "");
    audio_play("dial_0", 0.2f, 0, 1.0f);
    audio_play("whoosh_0", 0.1f, 0, 1.0f);
    hud_ui_add_chat(NULL, "Calling K.P.D ...", "#fc03ec"); // v10: updateGameMessage(-1, null, text, 2)
    phone_timers_stop(); // a stale drop from a report that went out just now
    phone(1);
    g_vk.ring_timer = html_timer_add(PHONE_RING_FIRST_MS, phone_ring, NULL);
    NetKpdCallPacket req;
    memset(&req, 0, sizeof(req));
    req.tab = 0;
    req.uid = uid;
    send_ext(client, NET_EXT_KPD_CALL, &req, sizeof(req));
    g_vk.call_timer = html_timer_add(KPD_NO_ANSWER_MS, call_no_answer, NULL);
}

static void kpd_list_show(int code, int status, const char *message, const char *json, size_t json_len);

// policeRes(code, message, data) (v10 :216287): code 0 ends the call (the siren for a report,
// the busy tone for anything else), codes 1..3 fill #kpdCalls of the K.P.D popup.
void hud_votekick_on_kpd(Client *client, const NetKpdResponsePacket *res, const char *json, const size_t json_len) {
    (void) client;
    if (!res) return;
    char message[64], name[32];
    memcpy(message, res->message, sizeof(message));
    memcpy(name, res->name, sizeof(name));
    message[sizeof(message) - 1] = name[sizeof(name) - 1] = 0;

    if (res->code == 0) { // the answer (v10 policeRes 0 :216063): the call is over
        if (g_vk.call_timer) html_timer_cancel(g_vk.call_timer);
        g_vk.call_timer = 0;
        g_vk.calling = 0;
        if (g_vk.ring_timer) html_timer_cancel(g_vk.ring_timer);
        g_vk.ring_timer = 0;
        audio_stop("ring_0");
        if (res->status == NET_KPD_REPORTED) {
            audio_play("siren_1", 0.1f, 0, 1.0f);
            if (!message[0]) snprintf(message, sizeof(message), "Reported %s to the K.P.D", name);
            g_vk.phone_reported = 1; // the picture blinks until the drop (v10 _71755 true, setTimeout 2200)
            g_vk.blink_on = 0;
            phone_blink(NULL);
            g_vk.drop_timer = html_timer_add(PHONE_DROP_REPORTED_MS, phone_drop, NULL);
        } else {
            audio_play("busy_0", 0.1f, 0, 1.0f);
            if (!message[0]) snprintf(message, sizeof(message), "The K.P.D could not take the call");
            phone_down(); // the busy line: the phone drops at once
            audio_play("whoosh_0", 0.1f, 0, 1.0f);
        }
        hud_ui_add_chat(NULL, message, "#fc03ec");
        return;
    }
    // an officer's list (v10 policeRes codes 1..5): only for the tab on show (`_71731 != code - 1`)
    if (res->code < 1 || res->code > 5 || res->code - 1 != ui_loadout_kpd_tab()) return;
    if (g_vk.list_timer && res->code == g_vk.list_wait) { // the answer is here: no "Request Failed"
        html_timer_cancel(g_vk.list_timer);
        g_vk.list_timer = 0;
    }
    int page = 0, pages = 0;
    if (sscanf(name, "%d/%d", &page, &pages) == 2 && pages > 1 && page >= 1 && page <= pages) {
        // a long list comes in pages (accounts.c acc_kpd_send_list): joined, rendered with the last
        if (page == 1 || g_vk.list_code != res->code || !g_vk.list_rows) {
            cJSON_Delete(g_vk.list_rows);
            g_vk.list_rows = cJSON_CreateArray();
            g_vk.list_code = res->code;
        }
        cJSON *part = json && json_len ? cJSON_ParseWithLength(json, json_len) : NULL;
        while (cJSON_IsArray(part) && g_vk.list_rows && cJSON_GetArraySize(part) > 0) cJSON_AddItemToArray(g_vk.list_rows, cJSON_DetachItemFromArray(part, 0));
        cJSON_Delete(part);
        if (page < pages) return;
        char *joined = g_vk.list_rows ? cJSON_PrintUnformatted(g_vk.list_rows) : NULL;
        cJSON_Delete(g_vk.list_rows);
        g_vk.list_rows = NULL;
        kpd_list_show(res->code, res->status, message, joined, joined ? strlen(joined) : 0);
        free(joined);
        return;
    }
    kpd_list_show(res->code, res->status, message, json, json_len);
}

// v10 _8991(ms): "[Ny ][Nd ][Nh ]Nm" (minutes always, no seconds)
static void kpd_duration(char *out, const size_t size, const double seconds) {
    long s = seconds > 0 ? (long) seconds : 0, m = s / 60, h = m / 60, d = h / 24;
    const long y = d / 365;
    m %= 60, h %= 24, d %= 365;
    size_t n = 0;
    out[0] = 0;
    if (y) n += (size_t) snprintf(out + n, size - n, "%ldy ", y);
    if (d && n < size) n += (size_t) snprintf(out + n, size - n, "%ldd ", d);
    if (h && n < size) n += (size_t) snprintf(out + n, size - n, "%ldh ", h);
    if (n < size) snprintf(out + n, size - n, "%ldm", m);
}

// v10's Distress Calls order: three stable sorts, the last by call type (1 ranked, 2 comp, public,
// 3 custom), the one before by the reporter's rating ((jop || 1) / (jor || 1): above 0.7 by helpful
// reports, 0.5..0.7 next, the rest last); the server's order (newest first) breaks ties.
typedef struct {
    const cJSON *row;
    int index, type_rank, rating_rank;
} KpdCallRow;

static double kpd_num(const cJSON *row, const char *key) {
    const cJSON *v = cJSON_GetObjectItem(row, key);
    return cJSON_IsNumber(v) ? v->valuedouble : 0.0;
}

static double kpd_ratio(const cJSON *row) {
    const double p = kpd_num(row, "jop"), t = kpd_num(row, "jor");
    return (p ? p : 1.0) / (t ? t : 1.0);
}

static int kpd_call_cmp(const void *a, const void *b) {
    const KpdCallRow *x = a, *y = b;
    if (x->type_rank != y->type_rank) return x->type_rank - y->type_rank;
    if (x->rating_rank != y->rating_rank) return y->rating_rank - x->rating_rank;
    return x->index - y->index;
}

// v10's rating colours (_71728) at its thresholds
static const char *kpd_rating_color(const double ratio) {
    return ratio <= 0.5 ? "#fc3232" : ratio <= 0.7 ? "#f7d31e" : "#63de26";
}

static void kpd_list_show(const int code, const int status, const char *message, const char *json, const size_t json_len) {
    const HtmlElem holder = html_dom_by_id("kpdCalls");
    if (!holder) return;
    char *html = hud_kpd_list_html(code, status, message, json, json_len);
    if (html) html_dom_set_html(holder, html);
    free(html);
}

// v10 policeRes codes 1 (Active Calls), 2 (Ban Logs) and 3 (Stats), from the brain's JSON rows

// v10 _7340: a K.P.D badge's level from XP (policeRes 1): max(1, floor(0.03 * sqrt(xp)))
static int kpd_level(const cJSON *row, const char *key) {
    const int level = (int) (0.03 * sqrt(kpd_num(row, key)));
    return level > 1 ? level : 1;
}

// v10 _71794: "YYYY-MM-DD HH:MM" in local time (epoch seconds), "?" without a date
static void kpd_date_text(char *out, const size_t size, const double epoch) {
    const time_t t = (time_t) epoch;
    struct tm tm;
#ifdef _WIN32
    const int ok = epoch > 0 && !localtime_s(&tm, &t);
#else
    const int ok = epoch > 0 && localtime_r(&t, &tm) != NULL;
#endif
    if (!ok) snprintf(out, size, "?");
    else snprintf(out, size, "%04d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
}

// v10 _71795 (milliseconds): "M:SS"
static void kpd_length_text(char *out, const size_t size, const double ms) {
    const long s = ms > 0 ? (long) (ms / 1000.0) : 0;
    snprintf(out, size, "%ld:%02ld", s / 60, s % 60);
}

// v10 _71796: BANNED / RESOLVED / the open votes (Y green, N red, a dot when this officer voted) / CLOSED
static void kpd_demo_status(StrBuf *sb, const cJSON *d) {
    if (kpd_num(d, "ban")) {
        sb_append(sb, "<span style='color:#eb5656'>BANNED</span>");
    } else if (kpd_num(d, "rsl")) {
        sb_append(sb, "<span style='color:#fbc02d'>RESOLVED</span>");
    } else if (kpd_num(d, "opn")) {
        sb_appendf(sb, "<span style='color:#7eeb7a'>%dY</span> <span style='color:#eb5656'>%dN</span>", (int) kpd_num(d, "yes"), (int) kpd_num(d, "f_no"));
        if (cJSON_IsNumber(cJSON_GetObjectItem(d, "myv"))) sb_append(sb, " <span style='color:#fbc02d' title='You voted on this demo'>&#9679;</span>");
    } else {
        sb_append(sb, "<span style='color:#858585'>CLOSED</span>");
    }
}

static const char *kpd_call_type_prefix(const int typ) {
    return typ == 1 ? "[ranked] " : typ == 2 ? "[comp] " : typ == 3 ? "[custom] " : "";
}

// policeRes 1 (Active Calls, v10 :216110): the rows in v10's order, each with the reporter's and the
// suspect's K.P.D level badge, the reporter's and suspect's names, the age and the server's region
static void kpd_call_rows(StrBuf *sb, const cJSON *rows) {
    const int count = cJSON_IsArray(rows) ? cJSON_GetArraySize(rows) : 0;
    if (!count) {
        sb_append(sb, "No Active Distress Calls");
        return;
    }
    KpdCallRow *order = calloc((size_t) count, sizeof(*order));
    if (!order) return;
    int n = 0;
    const cJSON *r;
    cJSON_ArrayForEach(r, rows) {
        const double ratio = kpd_ratio(r);
        const int typ = (int) kpd_num(r, "typ");
        order[n] = (KpdCallRow) {.row = r, .index = n, .type_rank = typ == 1 ? 0 : typ == 2 ? 1 : typ == 3 ? 3 : 2,
                                 .rating_rank = ratio <= 0.5 ? 0 : ratio <= 0.7 ? 1 : 1 + (int) kpd_num(r, "jop")};
        n++;
    }
    qsort(order, (size_t) n, sizeof(*order), kpd_call_cmp);
    sb_append(sb, "<table style=\"width:100%\"><tr><th style=\"font-size: 16px;\">Rating</th>"
                  "<th style=\"font-size: 16px;\">Reporter</th><th style=\"font-size: 16px;\">Suspect</th>"
                  "<th style=\"font-size: 16px;\">Info</th><th style=\"font-size: 16px;\">Join</th></tr>");
    const double now = (double) time(NULL);
    for (int i = 0; i < n; i++) {
        r = order[i].row;
        const char *can = cJSON_GetStringValue(cJSON_GetObjectItem(r, "can")), *con = cJSON_GetStringValue(cJSON_GetObjectItem(r, "con"));
        const char *gco = cJSON_GetStringValue(cJSON_GetObjectItem(r, "gco"));
        const int type = (int) kpd_num(r, "typ");
        sb_appendf(sb, "<tr><td><span class='kpdRepLvl' style='color:%s'>%d/%d</span></td>", kpd_rating_color(kpd_ratio(r)),
                   (int) kpd_num(r, "jop"), (int) kpd_num(r, "jor"));
        sb_appendf(sb, "<td><span class='kpdRepLvl'>%d</span><a href='javascript:void(0)' onclick='openPlayerProfile(\"", kpd_level(r, "crs"));
        sb_append_esc(sb, can ? can : "");
        sb_appendf(sb, "\")' class='menuLink' style='font-size:16px'>%s", kpd_call_type_prefix(type));
        sb_append_esc(sb, can ? can : "");
        sb_append(sb, "</a></td>");
        if (kpd_num(r, "gst")) {
            sb_append(sb, "<td style='color: var(--red);'>Guest</td>");
        } else {
            sb_appendf(sb, "<td><span class='kpdRepLvl'>%d</span><a href='javascript:void(0)' onclick='openPlayerProfile(\"", kpd_level(r, "cos"));
            sb_append_esc(sb, con ? con : "");
            sb_append(sb, "\")' class='menuLink' style='font-size:16px;color: var(--red);'>");
            sb_append_esc(sb, con ? con : "");
            sb_append(sb, "</a></td>");
        }
        // Info: how long ago, and the server (v10 gco.split(':')[0]: KrunkNative's game code is node:lobby)
        char ago[48], server[64];
        const double tim = kpd_num(r, "tim");
        kpd_duration(ago, sizeof(ago), tim > 0 ? now - tim : 0);
        snprintf(server, sizeof(server), "%s", gco ? gco : "");
        server[strcspn(server, ":")] = 0;
        sb_appendf(sb, "<td><span style='color:#858585;margin-right:12px'>%s ago</span><span class='callRegion'>", ago);
        sb_append_esc(sb, server);
        sb_append(sb, "</span></td><td><span class='policeJoinB' onclick=\"policeJoin('");
        sb_append_esc(sb, gco ? gco : "");
        sb_append(sb, "')\">Join</span></td></tr>");
    }
    free(order);
    sb_append(sb, "</table>");
}

// policeRes 2 (Ban Logs, v10 :216122): the moderator (a link, or the gold "KPD Demo"), the action,
// the suspect (a red link) and when it was logged
static void kpd_log_rows(StrBuf *sb, const cJSON *rows) {
    if (!cJSON_IsArray(rows) || !cJSON_GetArraySize(rows)) {
        sb_append(sb, "No Data");
        return;
    }
    sb_append(sb, "<table style=\"width:100%\"><tr><th style=\"font-size: 16px;\">Moderator</th>"
                  "<th style=\"font-size: 16px;\">Action</th><th style=\"font-size: 16px;\">Suspect</th>"
                  "<th style=\"font-size: 16px;\">Time</th></tr>");
    const cJSON *r;
    cJSON_ArrayForEach(r, rows) {
        const char *modn = cJSON_GetStringValue(cJSON_GetObjectItem(r, "modn")), *hckn = cJSON_GetStringValue(cJSON_GetObjectItem(r, "hckn"));
        sb_append(sb, "<tr><td>");
        if (modn && !strcmp(modn, "KPD Demo")) {
            sb_append(sb, "<span style='font-size:16px;color:#fbc02d'>KPD Demo</span>");
        } else {
            sb_append(sb, "<a href='javascript:void(0)' onclick='openPlayerProfile(\"");
            sb_append_esc(sb, modn ? modn : "");
            sb_append(sb, "\")' class='menuLink' style='font-size:16px'>");
            sb_append_esc(sb, modn ? modn : "");
            sb_append(sb, "</a>");
        }
        sb_appendf(sb, "</td><td><span style='color:#858585'>%s</span></td><td>", kpd_num(r, "htag") ? "tagged" : "untagged");
        sb_append(sb, "<a href='javascript:void(0)' onclick='openPlayerProfile(\"");
        sb_append_esc(sb, hckn ? hckn : "");
        sb_append(sb, "\")' class='menuLink' style='font-size:16px;color: var(--red);'>");
        sb_append_esc(sb, hckn ? hckn : "");
        sb_append(sb, "</a></td>");
        // v10 _8962(dat, true) + " ago": "H:MM D Mon YYYY ago" (the brain's dat: epoch seconds)
        char when[48] = "None";
        const time_t dat = (time_t) kpd_num(r, "dat");
        struct tm tm;
        static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
#ifdef _WIN32
        const int ok = dat > 0 && !localtime_s(&tm, &dat);
#else
        const int ok = dat > 0 && localtime_r(&dat, &tm) != NULL;
#endif
        if (ok) snprintf(when, sizeof(when), "%d:%02d %d %s %d", tm.tm_hour, tm.tm_min, tm.tm_mday, months[tm.tm_mon % 12], tm.tm_year + 1900);
        sb_appendf(sb, "</td><td><span style='color:#858585;'>%s ago</span></td></tr>", when);
    }
    sb_append(sb, "</table>");
}

// policeRes 3 (Stats, v10 :216130s): the tag counts of the officer's ranges (the first row's *Entries)
static void kpd_stats_rows(StrBuf *sb, const cJSON *rows) {
    static const char *const ranges[] = {"1day", "3day", "7day", "14day", "30day", "90day", "alltime"};
    const cJSON *first = cJSON_IsArray(rows) ? cJSON_GetArrayItem(rows, 0) : NULL;
    if (!first) {
        sb_append(sb, "No Data");
        return;
    }
    sb_append(sb, "<table style=\"width:100%\"><tr><th style=\"font-size: 16px;\">Range</th>"
                  "<th style=\"font-size: 16px;\">Tag Count</th></tr>");
    for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++) {
        char key[24];
        snprintf(key, sizeof(key), "%sEntries", ranges[i]);
        sb_appendf(sb, "<tr><td><span style='color:#858585'>%s</span></td><td><span style='color:#858585'>%d</span></td></tr>", ranges[i],
                   (int) kpd_num(first, key));
    }
    sb_append(sb, "</table>");
}

// policeRes 4 (Demos, v10 :216138): the player search, the suspects' table (a suspect's first demo
// carries the suspect cell; the rest of its group leave it empty) and Prev / Next. rows: the demo rows
// of one page (the trailer {"more": 1} is already taken out: more), off: the page's group offset, q: the search.
static void kpd_demo_rows(StrBuf *sb, const cJSON *rows, const int more, const int off, const char *q) {
    sb_append(sb, "<div style='margin-bottom:8px'><input id='kpdDemoQ' placeholder='Player Name' maxlength='20' value=\"");
    sb_append_esc(sb, q);
    sb_append(sb, "\" style='font-size:16px;padding:4px 8px;width:180px' onkeydown='if(event.key==\"Enter\")kpdDemoSearch()'/>"
                  "<span class='policeJoinB' style='margin-left:8px' onclick='kpdDemoSearch()'>Search</span>");
    if (q[0]) sb_append(sb, "<span class='policeJoinB' style='margin-left:8px' onclick='kpdDemoSearch(true)'>Clear</span>");
    sb_append(sb, "</div>");
    int count = 0;
    const cJSON *d;
    cJSON_ArrayForEach(d, rows) {
        if (cJSON_GetObjectItem(d, "tok")) count++;
    }
    if (count) {
        sb_append(sb, "<table style=\"width:100%\"><tr><th style=\"font-size: 16px;\">Suspect</th>"
                      "<th style=\"font-size: 16px;\">Match</th><th style=\"font-size: 16px;\">Date</th>"
                      "<th style=\"font-size: 16px;\">Status</th><th style=\"font-size: 16px;\">Watch</th></tr>");
        int prev_group = -1;
        cJSON_ArrayForEach(d, rows) {
            if (!cJSON_GetObjectItem(d, "tok")) continue;
            const int group = (int) kpd_num(d, "grp");
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(d, "name")), *tok = cJSON_GetStringValue(cJSON_GetObjectItem(d, "tok"));
            const char *ct = cJSON_GetStringValue(cJSON_GetObjectItem(d, "ct")), *map = cJSON_GetStringValue(cJSON_GetObjectItem(d, "map"));
            const char *mod = cJSON_GetStringValue(cJSON_GetObjectItem(d, "mod"));
            char date[32], length[16];
            kpd_date_text(date, sizeof(date), kpd_num(d, "dat"));
            kpd_length_text(length, sizeof(length), kpd_num(d, "dur"));
            sb_append(sb, "<tr style='vertical-align:top'>");
            if (group != prev_group) { // the suspect cell: a red link when banned (name), else "Suspect #grp"
                sb_append(sb, "<td style='line-height:1.8'>");
                if (kpd_num(d, "ban") && name && name[0]) {
                    sb_append(sb, "<a href='javascript:void(0)' onclick='openPlayerProfile(\"");
                    sb_append_esc(sb, name);
                    sb_append(sb, "\")' class='menuLink' style='font-size:16px;color:var(--red)'>");
                    sb_append_esc(sb, name);
                    sb_append(sb, "</a>");
                } else {
                    sb_appendf(sb, "<span style='font-size:16px'>Suspect #%d</span>", group);
                }
                if (kpd_num(d, "gst")) sb_append(sb, " <span style='color:#858585;font-size:13px'>guest</span>");
                sb_append(sb, "</td>");
            } else {
                sb_append(sb, "<td></td>");
            }
            prev_group = group;
            sb_append(sb, "<td style='line-height:1.8'><span style='font-size:16px'>");
            sb_append(sb, kpd_call_type_prefix((int) kpd_num(d, "typ")));
            sb_append_esc(sb, map ? map : "?");
            sb_append(sb, "</span><div style='color:#858585'>");
            sb_append_esc(sb, mod ? mod : "?");
            sb_append(sb, "</div></td><td style='line-height:1.8'><span style='color:#858585'>");
            sb_append(sb, date);
            sb_append(sb, "</span><div><span class='callRegion'>");
            sb_append(sb, length);
            sb_append(sb, "</span></div></td><td style='line-height:1.8'>");
            kpd_demo_status(sb, d);
            sb_append(sb, "</td><td style='white-space:nowrap'><span class='policeJoinB' onclick=\"watchDemo('");
            sb_append_esc(sb, tok);
            sb_append(sb, "','");
            sb_append_esc(sb, ct ? ct : "");
            sb_append(sb, "')\">Watch</span> <span class='material-icons' style='font-size:17px;vertical-align:middle;cursor:pointer;color:#fff'"
                          " title='Copy demo link' onclick=\"copyDemoLink(this,'");
            sb_append_esc(sb, tok);
            sb_append(sb, "','");
            sb_append_esc(sb, ct ? ct : "");
            sb_append(sb, "')\">content_copy</span></td></tr>");
        }
        sb_append(sb, "</table>");
    } else {
        sb_append(sb, q[0] ? "No Demos for that Player" : off ? "No More Demos" : "No Recorded Demos");
    }
    if (off || more) {
        sb_append(sb, "<div style='margin-top:10px'>");
        if (off) sb_append(sb, "<span class='policeJoinB' onclick='kpdDemoPage(-1)'>Prev</span>");
        if (more) sb_append(sb, "<span class='policeJoinB' style='margin-left:8px' onclick='kpdDemoPage(1)'>Next</span>");
        sb_append(sb, "</div>");
    }
}

// policeRes 5 (Reviewers, v10 :216165): the last 30 days and all time side by side, # / Reviewer / Reviews
static void kpd_reviewer_rows(StrBuf *sb, const cJSON *rows) {
    static const char *const titles[] = {"Last 30 Days", "All Time"}, *const ranges[] = {"month", "all"};
    sb_append(sb, "<div style='display:flex;gap:30px'>");
    for (int t = 0; t < 2; t++) {
        sb_appendf(sb, "<div style='flex:1'><div style='font-size:18px;margin-bottom:6px'>%s</div>", titles[t]);
        int count = 0;
        const cJSON *r;
        cJSON_ArrayForEach(r, rows) {
            const char *range = cJSON_GetStringValue(cJSON_GetObjectItem(r, "range"));
            if (range && !strcmp(range, ranges[t])) count++;
        }
        if (!count) {
            sb_append(sb, "<span style='color:#858585'>No Data</span></div>");
            continue;
        }
        sb_append(sb, "<table style=\"width:100%\"><tr><th style=\"font-size: 16px;\">#</th>"
                      "<th style=\"font-size: 16px;\">Reviewer</th><th style=\"font-size: 16px;\">Reviews</th></tr>");
        int index = 0;
        cJSON_ArrayForEach(r, rows) {
            const char *range = cJSON_GetStringValue(cJSON_GetObjectItem(r, "range"));
            if (!range || strcmp(range, ranges[t])) continue;
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(r, "name"));
            sb_appendf(sb, "<tr><td><span style='color:#858585'>%d</span></td><td><a href='javascript:void(0)' onclick='openPlayerProfile(\"", ++index);
            sb_append_esc(sb, name ? name : "");
            sb_append(sb, "\")' class='menuLink' style='font-size:16px'>");
            sb_append_esc(sb, name ? name : "");
            sb_appendf(sb, "</a></td><td><span style='color:#858585'>%d</span></td></tr>", (int) kpd_num(r, "cnt"));
        }
        sb_append(sb, "</table></div>");
    }
    sb_append(sb, "</div>");
}

// The list's HTML for #kpdCalls (policeRes codes 1..5). A failed request says its message (v10 _71741,
// "Request Failed" by default). rows: the parsed JSON array of the list; codes 4 and 5 take the trailer
// {"more": 1} of the demos. Malloc'd.
char *hud_kpd_list_html(const int code, const int status, const char *message, const char *json, const size_t json_len) {
    cJSON *rows = json && json_len ? cJSON_ParseWithLength(json, json_len) : NULL;
    StrBuf sb = {0};
    if (status != NET_KPD_REPORTED) {
        sb_append_esc(&sb, message && message[0] ? message : "Request Failed");
    } else if (code == 1) {
        kpd_call_rows(&sb, rows);
    } else if (code == 2) {
        kpd_log_rows(&sb, rows);
    } else if (code == 3) {
        kpd_stats_rows(&sb, rows);
    } else if (code == 4) {
        int more = 0;
        cJSON *d = NULL;
        cJSON_ArrayForEach(d, rows) {
            if (cJSON_GetObjectItem(d, "more")) more = (int) kpd_num(d, "more");
        }
        int off = 0;
        const char *q = "";
        ui_loadout_kpd_demo_query(&off, &q);
        kpd_demo_rows(&sb, rows, more, off, q);
    } else if (code == 5) {
        kpd_reviewer_rows(&sb, rows);
    } else {
        sb_append(&sb, "No Data");
    }
    cJSON_Delete(rows);
    return sb_finish(&sb);
}

// KrunkNative: a list request nothing answered in KPD_NO_ANSWER_MS (a server that refuses it in
// silence, a dropped brain request) says v10's failed-request text (_71741) instead of "Loading..."
// for good, if its tab is still the one on show.
static void list_no_answer(void *user) {
    (void) user;
    g_vk.list_timer = 0;
    if (g_vk.list_wait - 1 != ui_loadout_kpd_tab()) return;
    cJSON_Delete(g_vk.list_rows); // pages that came before the rest went missing
    g_vk.list_rows = NULL;
    const HtmlElem holder = html_dom_by_id("kpdCalls");
    if (holder) html_dom_set_html(holder, "Request Failed");
}

void hud_votekick_kpd_request(Client *client, const int tab) {
    if (g_vk.list_timer) html_timer_cancel(g_vk.list_timer);
    g_vk.list_timer = 0;
    if (g_scenario_lists) return;
    if (!client || !call_reachable(client)) { // no session: no answer can come (the list would say Loading... for good)
        kpd_list_show(tab, NET_KPD_UNAVAILABLE, "Not connected to a server", NULL, 0);
        return;
    }
    NetKpdCallPacket req;
    memset(&req, 0, sizeof(req));
    req.tab = (uint8_t) tab;
    req.uid = -1;
    send_ext(client, NET_EXT_KPD_CALL, &req, sizeof(req));
    g_vk.list_wait = tab;
    g_vk.list_timer = html_timer_add(KPD_NO_ANSWER_MS, list_no_answer, NULL);
}

// "kpd" tab 4 (Demos: v10 kpd.listDemos {off, q}) / tab 5 (Reviewers: kpd.reviewers()) as NET_EXT_KPD_LIST.
// The answer is policeRes code 4 / 5 (NET_EXT_KPD_RESPONSE, joined pages).
void hud_votekick_kpd_list(Client *client, const int code, const int off, const char *q) {
    if (g_vk.list_timer) html_timer_cancel(g_vk.list_timer);
    g_vk.list_timer = 0;
    if (g_scenario_lists) return;
    if (!client || !call_reachable(client)) {
        kpd_list_show(code, NET_KPD_UNAVAILABLE, "Not connected to a server", NULL, 0);
        return;
    }
    NetKpdListPacket req;
    memset(&req, 0, sizeof(req));
    req.tab = (uint8_t) code;
    req.off = off;
    snprintf(req.q, sizeof(req.q), "%s", q ? q : "");
    send_ext(client, NET_EXT_KPD_LIST, &req, sizeof(req));
    g_vk.list_wait = code;
    g_vk.list_timer = html_timer_add(KPD_NO_ANSWER_MS, list_no_answer, NULL);
}

// watchDemo(tok, ct) (v10 :222511): the demo's download. KrunkNative keeps no demo files, so the
// download fails with v10's own response popup (_69540(null, "Demo Download Failed")).
void hud_votekick_watch_demo(const char *tok, const char *ct) {
    (void) tok;
    (void) ct;
    ui_loadout_response_popup("Demo Download Failed");
}

// copyDemoLink(el, tok, ct) (v10 :222653): "?demo=<tok>&sus=<ct>" to the clipboard (the page's origin is
// the one this client runs under; KrunkNative has no demo page to open it on yet).
void hud_votekick_copy_demo_link(const char *tok, const char *ct) {
    char link[96];
    snprintf(link, sizeof(link), "?demo=%s%s%s", tok ? tok : "", ct && ct[0] ? "&sus=" : "", ct ? ct : "");
    html_clipboard_write(link);
}

// The player list's Kick / Ban (v10 ADMIN_ACTIONS): one NET_EXT_ADMIN_ACTION. The button shows only
// for DEV / the host; the server re-checks the sender's DEV right and the target on every request.
void hud_votekick_admin_action(Client *client, const int uid, const int action) {
    if (!client || uid < 0) return;
    NetAdminActionPacket req = {.uid = uid, .action = (uint8_t) action};
    send_ext(client, NET_EXT_ADMIN_ACTION, &req, sizeof(req));
}

// ---------------------------------------------------------------- the K.P.D officer (kpdData)

int hud_votekick_kpd_sus(const Client *client) {
    if (!g_vk.kpd_case || !client || client->net_client_id != g_vk.kpd_conn) return -1;
    return g_vk.kpd_sus;
}

int hud_votekick_kpd_caller(const Client *client) {
    if (!g_vk.kpd_case || !client || client->net_client_id != g_vk.kpd_conn) return -1;
    return g_vk.kpd_caller;
}

static const char *player_name_of(const Client *client, const int uid) {
    for (size_t i = 0; client && client->game.players && i < client->game.player_count; i++) {
        const Player *p = client->game.players[i];
        if (p && p->uid == uid) return p->name;
    }
    return NULL;
}

// v10 iiïíïíì(step): the spectator panel's verdict steps; -1 / 2 send "rkpd" 0 / 1 once
static void kpd_step(Client *client, const int step) {
    if (g_vk.kpd_done) return;
    if (step == 0) g_vk.kpd_step = 0;
    g_vk.kpd_step += step;
    const char *name = player_name_of(client, g_vk.kpd_sus);
    char esc[96], html[256];
    size_t len = 0;
    esc[0] = 0;
    append_esc(esc, sizeof(esc), &len, name ? name : "Suspect");
    const HtmlElem txt = html_dom_by_id("specKPDTxt");
    if (g_vk.kpd_step <= -1 || g_vk.kpd_step >= 2) {
        const int tagged = g_vk.kpd_step >= 2;
        if (tagged) snprintf(html, sizeof(html), "Case submitted!<div>Suspect Tagged</div>%s", name ? "<div>Profile URL Copied to Clipboard</div>" : "");
        else snprintf(html, sizeof(html), "Case submitted!<div>Suspect Not Tagged</div>");
        if (txt) html_dom_set_html(txt, html);
        dom_display("specKRHid", "none");
        if (tagged && name) { // v10 copies https://krunker.io/?profile=<name>
            char url[160], q[96];
            size_t k = 0;
            for (const char *c = name; *c && k + 4 < sizeof(q); c++) {
                if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || strchr("-_.~", *c)) q[k++] = *c;
                else k += (size_t) snprintf(q + k, sizeof(q) - k, "%%%02X", (unsigned char) *c);
            }
            q[k] = 0;
            snprintf(url, sizeof(url), "https://krunker.io/?profile=%s", q);
            html_clipboard_write(url);
        }
        const NetKpdRatePacket rate = {.tagged = (uint8_t) tagged};
        send_ext(client, NET_EXT_KPD_RATE, &rate, sizeof(rate));
        g_vk.kpd_done = 1;
        return;
    }
    if (g_vk.kpd_step == 0) snprintf(html, sizeof(html), "Is %s hacking?", esc);
    else snprintf(html, sizeof(html), "Are you 100%% sure?");
    if (txt) html_dom_set_html(txt, html);
    dom_display("specKRHid", "block");
}

static void kpd_panel(Client *client) {
    // startGame: kpdData ? (the KPD Vision row shown, iiïíïíì(0)) : specKPDContr hidden
    const HtmlElem contr = html_dom_by_id("specControlHolder");
    HtmlElem items[12];
    const int n = contr ? html_dom_query_all(contr, ".specControl", items, 12) : 0;
    if (n > 8) html_dom_set_display(items[8], g_vk.kpd_case ? "" : "none");
    if (!g_vk.kpd_case) {
        dom_display("specKPDContr", "none");
        return;
    }
    dom_display("specKPDContr", "block");
    kpd_step(client, 0);
}

void hud_votekick_on_kpd_data(Client *client, const NetKpdDataPacket *data) {
    if (!data || !client) return;
    g_vk.kpd_case = data->sus >= 0;
    g_vk.kpd_conn = client->net_client_id;
    g_vk.kpd_sus = data->sus;
    g_vk.kpd_caller = data->caller;
    g_vk.kpd_done = 0;
    kpd_panel(client);
    if (g_vk.kpd_case && spectate_active() && spectate_focused_uid() != data->sus) spectate_focus_player(client, data->sus, 1); // "Auto Focusing Suspect"
    refresh_player_list();
}

void hud_votekick_on_kpd_reset(Client *client) { // "kpdf": iiíìîìi
    g_vk.kpd_done = 0;
    if (g_vk.kpd_case) kpd_step(client, 0);
}

// kpdVoteY / kpdVoteN while spectating with a case (v10 :221340): 1 when it took the key
int hud_votekick_kpd_key(Client *client, const int yes) {
    if (hud_votekick_kpd_sus(client) < 0 || !spectate_active()) return 0;
    kpd_step(client, yes ? 1 : -1);
    return 1;
}

// hud_spectate.c: the panel's display follows the case
int hud_votekick_kpd_panel_visible(const Client *client) { return hud_votekick_kpd_sus(client) >= 0; }

// ---------------------------------------------------------------- session

// INIT: a new session or round (v10 resets voteToKick with the game; the voted list and the
// initiated flag are per page load there, per session here)
void hud_votekick_reset(void) {
    g_vk.active = 0;
    g_vk.target[0] = 0;
    g_vk.initiator[0] = 0;
    g_vk.votes = g_vk.needed = 0;
    g_vk.initiated = 0;
    g_vk.voted_count = 0;
    call_end(); // a call in flight does not survive the round: its answer would come too late to mean anything
    dom_display("voteToKick", "none");
    dom_display("voteToForfeit", "none"); // a new round: no forfeit vote either
}

// The session ended under a call (a disconnect, a failed reconnect, a server switch): no answer can
// reach this page any more, so the phone comes down now rather than at the timeout. The call ends
// silently; the connection screen already says what happened.
void hud_votekick_on_session_lost(void) {
    if (g_vk.calling) call_end();
}

// --render-scenario state.kpdPhone: the phone up with a call in flight and no request sent (the
// captures of the phone; an answer from the offline sandbox would take it down at once)
// --render-scenario state.kpdPolice: the officer's lists are answered from the fixture, no request goes out
void hud_votekick_scenario_lists(const int on) { g_scenario_lists = on != 0; }

// the fixture: policeRes(code, "", json) as the server would answer the list (one page)
void hud_votekick_scenario_list(Client *client, const int code, const char *json) {
    NetKpdResponsePacket r;
    memset(&r, 0, sizeof(r));
    r.code = (uint8_t) code;
    r.status = NET_KPD_REPORTED;
    hud_votekick_on_kpd(client, &r, json, json ? strlen(json) : 0);
}

void hud_votekick_scenario_phone(const int up) {
    if (up) {
        g_vk.calling = 1;
        phone(1);
    } else {
        call_end();
    }
}

int hud_votekick_active(void) { return g_vk.active; }
const char *hud_votekick_target(void) { return g_vk.target; }

// ---------------------------------------------------------------- handlers (hud_ui_handle)

static int arg_int(const HtmlArg *args, const int argc, const int i, const int def) {
    if (i >= argc) return def;
    if (args[i].kind == HTML_ARG_NUM) return (int) args[i].num;
    if (args[i].kind == HTML_ARG_STR && args[i].str) return atoi(args[i].str);
    return def;
}

static const char *arg_text(const HtmlArg *args, const int argc, const int i) {
    return i < argc && args[i].kind == HTML_ARG_STR && args[i].str ? args[i].str : "";
}

int hud_votekick_handle(Client *client, const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    if (!name) return -1;
    if (!strcmp(name, "kickVote")) { // kickVote(id)
        kick_vote(client, arg_int(args, argc, 0, -1));
        return 1;
    }
    if (!strcmp(name, "confirmPolice")) { // confirmPolice(alias, id)
        confirm_police(arg_text(args, argc, 0), arg_int(args, argc, 1, -1));
        return 1;
    }
    if (!strcmp(name, "callPolice")) { // callPolice(alias, id)
        call_police(client, arg_text(args, argc, 0), arg_int(args, argc, 1, -1));
        return 1;
    }
    if (!strcmp(name, "copyUser")) { // copyUser(elm, id): the player's id to the clipboard
        char text[32];
        snprintf(text, sizeof(text), "%d", arg_int(args, argc, 1, arg_int(args, argc, 0, -1)));
        html_clipboard_write(text);
        return 0; // no context menu
    }
    if (!strcmp(name, "policeJoin")) { // policeJoin(gameId, ...): the siren, switchServer(id)
        audio_play("siren_1", 0.1f, 0, 1.0f);
        servers_ui_join_game(arg_text(args, argc, 0));
        return 1;
    }
    if (!strcmp(name, "switchKPDTab")) { // switchKPDTab(i): the officers' tab i, showPolicePop() again (ui_loadout.c)
        ui_loadout_switch_kpd_tab(arg_int(args, argc, 0, 0));
        return 1;
    }
    return -1;
}
