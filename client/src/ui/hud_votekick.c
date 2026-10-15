// The player list's actions (Krunker v10.0.2 game.deob.js, windows[22] genList :214300): the
// Votekick button and the vote prompt (#voteToKick: kickVote :220193, io "vk" :219974, the
// kickVoteY / kickVoteN keys :221373), the K.P.D button with its confirm popup and call
// (confirmPolice / callPolice :216268-216274, policeRes code 0 :216287) and copyUser. The
// packets are NET_PACKET_EXT subtypes (shared/include/net_ext.h 0x0800..); the rules are the
// server's (server/src/votekick.c).
#include <ui_logic.h>
#include <client.h>
#include <html_dom.h>
#include <html_ui.h>
#include <hud_social.h>
#include <player_accounts.h>
#include <backend_ui.h> // account_get: v10 shows Votekick / K.P.D to accounts only; servers_ui_join_game (policeJoin)
#include <audio.h>
#include <net_ext.h>
#include <game_config.h> // game_config_is_custom: v10 isCustom
#include <spectate.h> // the officer's suspect: focusPlayer(sus, false, true)
#include <pthread.h> // client->net_lock: the socket check before a call goes out
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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
    // K.P.D officer (v10 kpdData {sus, caller}, NET_EXT_KPD_DATA): the case of this connection
    int kpd_case;              // a case (iííiìíî != null)
    int kpd_conn;              // client->net_client_id it came on (a new connection: no case)
    int kpd_sus, kpd_caller;   // the suspect's / reporter's uid (-1 none)
    int kpd_step;              // iìíîiíi: -1 not tagged, 0 "Is X hacking?", 1 "Are you 100% sure?", 2 tagged
    int kpd_done;              // iîíìiïi: the verdict went out ("rkpd")
    // --render-scenario / tests: the rows come from here instead of the live game
    cJSON *override;
} g_vk;

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
    if (!client || !account_get() || !alias || !alias[0]) return 0;
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
    const AccountInfo *acc = account_get();
    return client && acc && acc->level >= 5;
}

// ---------------------------------------------------------------- the prompt (io "vk")

static void show_prompt(const Client *client, const int opened) {
    char html[160];
    size_t len = 0;
    html[0] = 0;
    if (opened) { // "<voter> voted to kick <target>"
        append_esc(html, sizeof(html), &len, g_vk.initiator);
        append_esc(html, sizeof(html), &len, " voted to kick ");
        append_esc(html, sizeof(html), &len, g_vk.target);
        dom_html("voteKickInfo", html);
        len = 0;
        html[0] = 0;
        append_esc(html, sizeof(html), &len, "Kick ");
        append_esc(html, sizeof(html), &len, g_vk.target);
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

// kickVoteY / kickVoteN (v10 :221373): only while a vote runs, the player is alive, and they have
// not voted on this target yet. [1] sends v2k(target, 1), [2] v2k(target) and hides the prompt.
int hud_votekick_key(Client *client, const int yes) {
    if (!g_vk.active || !client || !client->me || !client->me->active || !account_get()) return 0;
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
    char esc[96];
    size_t len = 0;
    esc[0] = 0;
    append_esc(esc, sizeof(esc), &len, alias);
    char html[900];
    snprintf(html, sizeof(html),
             "<div style='color:rgba(255,255,255,0.6);font-size:20px;margin-bottom:15px;'>Only call the <b>K.P.D</b> if you strongly suspect <b>%s</b>"
             " of using Hacks. <div style='height:10px'></div>False Reports can result in a Ban or Loss of Perms.</div>"
             "<div id='confirmBtn' style='width:142px;margin-right:5px' onclick='callPolice(`%s`,`%d`)'>Call<span class='material-icons'"
             " style='color:#fff;margin-left:10px;font-size:28px;vertical-align:bottom;margin-bottom:1px'>call</span></div>"
             "<div id='declineBtn' style='width:142px;margin-left:3px' onclick='clearPops()'>Cancel</div>",
             esc, esc, uid);
    ui_loadout_clear_pops();
    dom_display("popupHolder", "block");
    const HtmlElem pop = html_dom_by_id("genericPop");
    if (!pop) return;
    html_dom_set_attr(pop, "class", "polConfPop");
    html_dom_set_attr(pop, "style", "");
    html_dom_set_html(pop, html);
    html_dom_set_display(pop, "block");
}

static void phone(const int up) {
    const HtmlElem img = html_dom_by_id("phoneImg");
    if (img) html_dom_set_style(img, "margin-bottom", up ? "0px" : "-700px");
}

// The call is over: the phone down, the no-answer timer cancelled (so it cannot end a later call)
// and no call in flight. Every way out of a call comes through here: the answer, the timeout, a
// new round (INIT) and a lost session.
static void call_end(void) {
    if (g_vk.call_timer) html_timer_cancel(g_vk.call_timer);
    g_vk.call_timer = 0;
    g_vk.calling = 0;
    phone(0);
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
    phone(1);
    NetKpdCallPacket req;
    memset(&req, 0, sizeof(req));
    req.tab = 0;
    req.uid = uid;
    send_ext(client, NET_EXT_KPD_CALL, &req, sizeof(req));
    g_vk.call_timer = html_timer_add(KPD_NO_ANSWER_MS, call_no_answer, NULL);
}

// policeRes(code, message, data) (v10 :216287): code 0 ends the call (the siren for a report,
// the busy tone for anything else), codes 1..3 fill #kpdCalls of the K.P.D popup.
void hud_votekick_on_kpd(Client *client, const NetKpdResponsePacket *res, const char *json, const size_t json_len) {
    (void) client;
    if (!res) return;
    char message[64], name[32];
    memcpy(message, res->message, sizeof(message));
    memcpy(name, res->name, sizeof(name));
    message[sizeof(message) - 1] = name[sizeof(name) - 1] = 0;
    if (res->code == 0) {
        call_end(); // the phone down with the answer
        audio_stop("ring_0");
        if (res->status == NET_KPD_REPORTED) {
            audio_play("siren_1", 0.1f, 0, 1.0f);
            if (!message[0]) snprintf(message, sizeof(message), "Reported %s to the K.P.D", name);
        } else {
            audio_play("busy_0", 0.1f, 0, 1.0f);
            if (!message[0]) snprintf(message, sizeof(message), "The K.P.D could not take the call");
        }
        hud_ui_add_chat(NULL, message, "#fc03ec");
        audio_play("whoosh_0", 0.1f, 0, 1.0f);
        return;
    }
    // an officer's list: the K.P.D popup's #kpdCalls (hud_kpd_list_html renders v10's table)
    const HtmlElem holder = html_dom_by_id("kpdCalls");
    if (!holder) return;
    char *html = hud_kpd_list_html(res->code, res->status, message, json, json_len);
    if (html) html_dom_set_html(holder, html);
    free(html);
}

// v10 policeRes codes 1 (Distress Calls) and 2 (the moderation log), from the brain's JSON rows
char *hud_kpd_list_html(const int code, const int status, const char *message, const char *json, const size_t json_len) {
    cJSON *rows = json && json_len ? cJSON_ParseWithLength(json, json_len) : NULL;
    const int count = cJSON_IsArray(rows) ? cJSON_GetArraySize(rows) : 0;
    size_t cap = 1024 + (size_t) count * 512;
    char *out = malloc(cap);
    if (!out) {
        cJSON_Delete(rows);
        return NULL;
    }
    size_t len = 0;
    out[0] = 0;
    if (status != NET_KPD_REPORTED) {
        append_esc(out, cap, &len, message && message[0] ? message : "Request Failed");
    } else if (!count) {
        append_esc(out, cap, &len, code == 1 ? "No Active Distress Calls" : "No Data");
    } else if (code == 1) {
        len += (size_t) snprintf(out + len, cap - len, "<table style=\"width:100%%\"><tr><th style=\"font-size: 16px;\">Rating</th>"
                                                       "<th style=\"font-size: 16px;\">Reporter</th><th style=\"font-size: 16px;\">Suspect</th>"
                                                       "<th style=\"font-size: 16px;\">Info</th><th style=\"font-size: 16px;\">Join</th></tr>");
        const cJSON *r;
        cJSON_ArrayForEach(r, rows) {
            const cJSON *jop = cJSON_GetObjectItem(r, "jop"), *jor = cJSON_GetObjectItem(r, "jor"), *typ = cJSON_GetObjectItem(r, "typ");
            const cJSON *gst = cJSON_GetObjectItem(r, "gst");
            const char *can = cJSON_GetStringValue(cJSON_GetObjectItem(r, "can")), *con = cJSON_GetStringValue(cJSON_GetObjectItem(r, "con"));
            const char *gco = cJSON_GetStringValue(cJSON_GetObjectItem(r, "gco"));
            const double p = cJSON_IsNumber(jop) ? jop->valuedouble : 0, t = cJSON_IsNumber(jor) ? jor->valuedouble : 0;
            const double ratio = t > 0 ? p / t : 1.0;
            const char *col = ratio <= 0.5 ? "#eb5656" : ratio <= 0.7 ? "#fbc02d" : "#7eeb7a";
            const int type = cJSON_IsNumber(typ) ? typ->valueint : 0;
            len += (size_t) snprintf(out + len, cap - len, "<tr><td><span class='kpdRepLvl' style='color:%s'>%d/%d</span></td><td>", col, (int) p, (int) t);
            if (len + 400 > cap) break;
            append_esc(out, cap, &len, type == 1 ? "[ranked] " : type == 2 ? "[comp] " : type == 3 ? "[custom] " : "");
            len += (size_t) snprintf(out + len, cap - len, "<a href='javascript:void(0)' onclick='openPlayerProfile(\"");
            append_esc(out, cap, &len, can ? can : "");
            len += (size_t) snprintf(out + len, cap - len, "\")' class='menuLink' style='font-size:16px'>");
            append_esc(out, cap, &len, can ? can : "");
            len += (size_t) snprintf(out + len, cap - len, "</a></td>");
            if (cJSON_IsNumber(gst) && gst->valueint) {
                len += (size_t) snprintf(out + len, cap - len, "<td style='color: var(--red);'>Guest</td>");
            } else {
                len += (size_t) snprintf(out + len, cap - len, "<td><a href='javascript:void(0)' onclick='openPlayerProfile(\"");
                append_esc(out, cap, &len, con ? con : "");
                len += (size_t) snprintf(out + len, cap - len, "\")' class='menuLink' style='font-size:16px;color: var(--red);'>");
                append_esc(out, cap, &len, con ? con : "");
                len += (size_t) snprintf(out + len, cap - len, "</a></td>");
            }
            len += (size_t) snprintf(out + len, cap - len, "<td><span class='callRegion'>");
            append_esc(out, cap, &len, gco ? gco : "");
            len += (size_t) snprintf(out + len, cap - len, "</span></td><td><span class='policeJoinB' onclick=\"policeJoin('");
            append_esc(out, cap, &len, gco ? gco : "");
            len += (size_t) snprintf(out + len, cap - len, "')\">Join</span></td></tr>");
        }
        len += (size_t) snprintf(out + len, cap - len, "</table>");
    } else {
        len += (size_t) snprintf(out + len, cap - len, "<table style=\"width:100%%\"><tr><th style=\"font-size: 16px;\">Moderator</th>"
                                                       "<th style=\"font-size: 16px;\">Action</th><th style=\"font-size: 16px;\">Suspect</th></tr>");
        const cJSON *r;
        cJSON_ArrayForEach(r, rows) {
            const char *modn = cJSON_GetStringValue(cJSON_GetObjectItem(r, "modn")), *hckn = cJSON_GetStringValue(cJSON_GetObjectItem(r, "hckn"));
            const cJSON *htag = cJSON_GetObjectItem(r, "htag");
            if (len + 300 > cap) break;
            len += (size_t) snprintf(out + len, cap - len, "<tr><td>");
            append_esc(out, cap, &len, modn ? modn : "");
            len += (size_t) snprintf(out + len, cap - len, "</td><td><span style='color:#858585'>%s</span></td><td style='color: var(--red);'>",
                                     cJSON_IsNumber(htag) && htag->valueint ? "tagged" : "untagged");
            append_esc(out, cap, &len, hckn ? hckn : "");
            len += (size_t) snprintf(out + len, cap - len, "</td></tr>");
        }
        len += (size_t) snprintf(out + len, cap - len, "</table>");
    }
    cJSON_Delete(rows);
    return out;
}

// The K.P.D popup's tabs for officers: "kpd" tab (1 calls, 2 log) -> policeRes(code)
void hud_votekick_kpd_request(Client *client, const int tab) {
    NetKpdCallPacket req;
    memset(&req, 0, sizeof(req));
    req.tab = (uint8_t) tab;
    req.uid = -1;
    send_ext(client, NET_EXT_KPD_CALL, &req, sizeof(req));
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
}

// The session ended under a call (a disconnect, a failed reconnect, a server switch): no answer can
// reach this page any more, so the phone comes down now rather than at the timeout. The call ends
// silently; the connection screen already says what happened.
void hud_votekick_on_session_lost(void) {
    if (g_vk.calling) call_end();
}

// --render-scenario state.kpdPhone: the phone up with a call in flight and no request sent (the
// captures of the phone; an answer from the offline sandbox would take it down at once)
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
    if (!strcmp(name, "switchKPDTab")) { // switchKPDTab(i): the officers' tabs (0 calls, 1 log)
        hud_votekick_kpd_request(client, arg_int(args, argc, 0, 0) + 1);
        return 1;
    }
    return -1;
}
