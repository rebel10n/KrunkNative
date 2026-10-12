// Krunker v3.1.1 ranked queue (index.js joinQueue / leaveQueue / initQueueSession /
// updateQueueStatus / getSelectedQueueIds / windows[26] = the Ranked window, and
// ClientManager.js QueueManager) over the brain's HTTP queue routes (doc/backend.md 6.5):
// /mm/queue/join replaces v3.1.1's game-socket "queue" + "iq"; /mm/queue/poll (1 s) and
// /mm/queue/update-* / kick / promote / leave are v3.1.1's ClientManager routes. A formed
// match ("Join" {gameId, host, port, webUrl, clientId}) is joined with its ticket.
#include <backend.h>
#include <backend_ui.h>
#include <ui_logic.h>
#include <html_ui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIN_RANKED_LEVEL 15 // config.minRankedLevel
#define JOIN_COUNTDOWN 5    // joinCountdownSeconds

static const struct {
    const char *id, *name;
    int max_party;
    int elo_slot; // data40 index of acc[key]
} k_queues[] = {{"r1v1", "Solo", 1, 22}, {"r2v2", "Duo", 2, 23}, {"r4v4", "Quad", 4, 24}};
#define QUEUE_COUNT 3

static char g_client_id[64], g_client_key[128]; // client.queueManager (empty = not queued)
static cJSON *g_party;            // party: the last poll response (owned)
static char g_status[96];         // queueStatusMessage ("" = null)
static char g_ready[32];          // queueReadyMessage
static int g_poll_timer, g_countdown_timer, g_countdown, g_countdown_started;
static double g_cant_queue;       // windows[26].cantQueue (ms), 0 = none
static int g_just_copied = -2;    // windows[26].justCopied (-2 = null)
static int g_initial_status;      // isInitialPartyStatus
static char g_new_game[160];      // QueueManager.newGameId

static const AccountInfo *acc(void) { return account_get(); }

static void enc(char *out, size_t n, const char *s) { backend_url_encode(out, n, s ? s : ""); }

static int party_is_leader(void) {
    const char *leader = cJSON_GetStringValue(cJSON_GetObjectItem(g_party, "partyLeaderId"));
    return !g_party || (g_client_id[0] && leader && !strcmp(leader, g_client_id));
}

// updateQueueStatus(override)
static void update_status(const char *override);

static void rerender(void) {
    menu_update_instructions();
    menu_rerender_if_open(27);
}

static void countdown_tick(void *user) {
    (void) user;
    g_countdown_timer = 0;
    if (--g_countdown > 0) {
        char t[32];
        snprintf(t, sizeof(t), "Joining in %d", g_countdown);
        update_status(t);
        g_countdown_timer = html_timer_add(1000, countdown_tick, NULL);
    } else {
        update_status("Joining game");
    }
}

static void update_status(const char *override) {
    if (g_party || override) {
        if (override) {
            snprintf(g_status, sizeof(g_status), "%s", override);
        } else {
            const cJSON *st = cJSON_GetObjectItem(g_party, "status");
            const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(st, "t"));
            const cJSON *c = cJSON_GetObjectItem(st, "c");
            const cJSON *qids = cJSON_GetObjectItem(g_party, "queueIds");
            const cJSON *members = cJSON_GetObjectItem(g_party, "partyMembers");
            int ready = 0, total = 0;
            const cJSON *m;
            cJSON_ArrayForEach(m, members) {
                total++;
                if (cJSON_IsTrue(cJSON_GetObjectItem(m, "isReady"))) ready++;
            }
            if (!t) t = "";
            if (!strcmp(t, "Queuing")) snprintf(g_status, sizeof(g_status), "Queuing");
            else if (!strcmp(t, "Queued")) {
                if (!cJSON_GetArraySize(qids)) snprintf(g_status, sizeof(g_status), "%s", party_is_leader() ? "No queues selected" : "Waiting for leader");
                else if (ready == total) snprintf(g_status, sizeof(g_status), "Finding match [%d in queue]", (int) cJSON_GetNumberValue(cJSON_GetObjectItem(c, "queueSize")));
                else snprintf(g_status, sizeof(g_status), "Party not ready");
            } else if (!strcmp(t, "CreatingGame") || !strcmp(t, "HostGame")) {
                if (!g_countdown_started) {
                    g_countdown_started = 1;
                    g_countdown = JOIN_COUNTDOWN + 1;
                    countdown_tick(NULL);
                    return;
                }
            } else if (!strcmp(t, "Join")) {
                snprintf(g_status, sizeof(g_status), "Joining game");
            } else {
                snprintf(g_status, sizeof(g_status), "?");
            }
        }
        if (g_party) {
            char *ids = cJSON_PrintUnformatted(cJSON_GetObjectItem(g_party, "queueIds"));
            if (ids) html_storage_set("krk_lastQueues", ids);
            free(ids);
            int ready = 0, total = 0;
            const cJSON *m;
            cJSON_ArrayForEach(m, cJSON_GetObjectItem(g_party, "partyMembers")) {
                total++;
                if (cJSON_IsTrue(cJSON_GetObjectItem(m, "isReady"))) ready++;
            }
            snprintf(g_ready, sizeof(g_ready), "%d/%d Ready", ready, total);
        } else {
            g_ready[0] = 0;
        }
        HtmlElem qs = html_dom_by_id("queueStatus");
        if (qs) html_dom_set_display(qs, "inline-block");
        HtmlElem base = html_dom_by_id("uiBase");
        if (base) html_dom_set_class(base, "isQueued", 1);
    } else {
        HtmlElem qs = html_dom_by_id("queueStatus");
        if (qs) html_dom_set_display(qs, "none");
        HtmlElem base = html_dom_by_id("uiBase");
        if (base) html_dom_set_class(base, "isQueued", 0);
        if (g_countdown_timer) html_timer_cancel(g_countdown_timer);
        g_countdown_timer = 0;
        g_status[0] = g_ready[0] = 0;
        g_countdown_started = 0;
    }
    rerender();
}

const char *ranked_ui_status(void) { return g_status[0] ? g_status : NULL; }
const char *ranked_ui_ready(void) { return g_ready[0] ? g_ready : NULL; }

static void stop_polling(void) {
    if (g_poll_timer) html_timer_cancel(g_poll_timer);
    g_poll_timer = 0;
}

// client.unqueue(): forget the session (the brain's leave is sent separately)
static void unqueue(void) {
    stop_polling();
    g_client_id[0] = g_client_key[0] = 0;
    g_new_game[0] = 0;
}

static void request(const char *route, const char *extra, BackendHttpFn fn);
static void on_poll(void *user, int status, const char *body, size_t length);

static void poll_fire(void *user) {
    (void) user;
    g_poll_timer = 0;
    if (!g_client_key[0]) return;
    request("/mm/queue/poll", "", on_poll);
}

static void request(const char *route, const char *extra, const BackendHttpFn fn) {
    char key[256], path[1024];
    enc(key, sizeof(key), g_client_key);
    snprintf(path, sizeof(path), "%s?clientKey=%s%s", route, key, extra ? extra : "");
    backend_http_get(path, fn, NULL);
}

// QueueManager._handleNewPollResponse(k)
static void handle_poll(cJSON *res) {
    if (!g_client_key[0]) {
        cJSON_Delete(res);
        return;
    }
    const cJSON *st = cJSON_GetObjectItem(res, "status");
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(st, "t"));
    const cJSON *c = cJSON_GetObjectItem(st, "c");
    if (t && !strcmp(t, "Join")) {
        const char *game = cJSON_GetStringValue(cJSON_GetObjectItem(c, "gameId"));
        if (game && strcmp(game, g_new_game)) {
            snprintf(g_new_game, sizeof(g_new_game), "%s", game);
            // joinCallback(gameId): unqueue + io.send("unqueue") + switchGame
            const char *host = cJSON_GetStringValue(cJSON_GetObjectItem(c, "host"));
            const int port = (int) cJSON_GetNumberValue(cJSON_GetObjectItem(c, "port"));
            const char *ticket = cJSON_GetStringValue(cJSON_GetObjectItem(c, "clientId"));
            const char *web = cJSON_GetStringValue(cJSON_GetObjectItem(c, "webUrl"));
            printf("[ranked] match ready: joining %s\n", game);
            request("/mm/queue/leave", "", NULL);
            unqueue();
            cJSON_Delete(g_party);
            g_party = NULL;
            update_status(NULL);
            menu_show_window(MENU_WINDOW_NONE, 0);
            if (host && port > 0) servers_ui_join_ticket(host, port, game, ticket ? ticket : "", web);
            cJSON_Delete(res);
            return;
        }
    }
    // statusCallback(res): a changed party re-renders
    char *a = g_party ? cJSON_PrintUnformatted(g_party) : NULL;
    char *b = cJSON_PrintUnformatted(res);
    const int changed = !a || !b || strcmp(a, b);
    free(a);
    free(b);
    if (changed) {
        g_initial_status = g_party == NULL;
        const char *pid = cJSON_GetStringValue(cJSON_GetObjectItem(res, "partyId"));
        if (pid) html_storage_set("krk_lastPartyId", pid);
        cJSON_Delete(g_party);
        g_party = res;
        res = NULL;
        update_status(NULL);
    }
    cJSON_Delete(res);
}

static void on_poll(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    (void) length;
    cJSON *res = cJSON_Parse(body);
    const char *err = cJSON_GetStringValue(cJSON_GetObjectItem(res, "error"));
    if (status != 200 && err && (!strcmp(err, "InvalidClientId") || !strcmp(err, "InvalidClientKey"))) {
        // kickCallback -> leaveQueue
        cJSON_Delete(res);
        unqueue();
        cJSON_Delete(g_party);
        g_party = NULL;
        update_status(NULL);
        return;
    }
    if (status == 200 && cJSON_IsObject(res)) handle_poll(res);
    else cJSON_Delete(res);
    if (g_client_key[0] && !g_poll_timer) g_poll_timer = html_timer_add(1000, poll_fire, NULL);
}

// initQueueSession(clientId, clientKey, status)
static void on_join(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    (void) length;
    cJSON *res = cJSON_Parse(body);
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(res, "clientId"));
    const char *key = cJSON_GetStringValue(cJSON_GetObjectItem(res, "clientKey"));
    const char *st = cJSON_GetStringValue(cJSON_GetObjectItem(res, "status"));
    if (status == 200 && id && key) {
        snprintf(g_client_id, sizeof(g_client_id), "%s", id);
        snprintf(g_client_key, sizeof(g_client_key), "%s", key);
        poll_fire(NULL); // QueueManager._start
    } else {
        cJSON_Delete(g_party);
        g_party = NULL;
        if (st && !strcmp(st, "RankedCooldown")) {
            g_cant_queue = cJSON_GetNumberValue(cJSON_GetObjectItem(res, "cooldownMs"));
            update_status(NULL);
        } else if (st && !strcmp(st, "AccountAlreadyPlaying")) {
            update_status("Already queued in another tab");
            html_timer_add(2000, (void (*)(void *)) update_status, NULL);
        } else {
            update_status(st ? st : "Failed");
            html_timer_add(2000, (void (*)(void *)) update_status, NULL);
        }
    }
    cJSON_Delete(res);
}

// getSelectedQueueIds()
static cJSON *selected_queues(void) {
    cJSON *ids = cJSON_CreateArray();
    HtmlElem boxes[8];
    const int n = html_dom_query_all(0, ".rankedQueueOption", boxes, 8);
    for (int i = 0; i < n; i++) {
        char id[16] = "";
        if (html_dom_get_checked(boxes[i]) && html_dom_get_attr(boxes[i], "data-queue-id", id, sizeof(id)) >= 0) cJSON_AddItemToArray(ids, cJSON_CreateString(id));
    }
    return ids;
}

// joinQueue(startReady, partyIdOverride)
static void join_queue(const int ready) {
    if (g_client_key[0] || !acc()) return;
    char party[128] = "", region[32] = "";
    HtmlElem key = html_dom_by_id("rankedPartyKey");
    if (key) html_dom_get_value(key, party, sizeof(party));
    char *p = strstr(party, "?party=");
    if (p) memmove(party, p + 7, strlen(p + 7) + 1);
    HtmlElem sel = html_dom_by_id("queueRegionSelect");
    if (sel) html_dom_get_value(sel, region, sizeof(region));
    if (!region[0] && html_storage_get("krunker_rankedRegion", region, sizeof(region)) < 0) region[0] = 0;
    if (!region[0] && html_storage_get("pingRegion7", region, sizeof(region)) < 0) region[0] = 0;
    if (!region[0]) snprintf(region, sizeof(region), "local");
    html_storage_set("krunker_rankedRegion", region);
    cJSON *ids = selected_queues();
    char *ids_text = cJSON_PrintUnformatted(ids);
    cJSON_Delete(ids);
    char e_token[256], e_region[96], e_queues[256], e_party[256], path[1400];
    enc(e_token, sizeof(e_token), account_session_token());
    enc(e_region, sizeof(e_region), region);
    enc(e_queues, sizeof(e_queues), ids_text ? ids_text : "[]");
    enc(e_party, sizeof(e_party), party);
    free(ids_text);
    snprintf(path, sizeof(path), "/mm/queue/join?token=%s&region=%s&queues=%s&party=%s&ready=%d&classIndex=%d", e_token, e_region, e_queues,
             e_party, ready, menu_ui()->client->menu_class_index);
    backend_http_get(path, on_join, NULL);
    update_status("Queuing");
}

// leaveQueue()
static void leave_queue(void) {
    if (!g_client_key[0]) return;
    request("/mm/queue/leave", "", NULL);
    unqueue();
    cJSON_Delete(g_party);
    g_party = NULL;
    update_status(NULL);
}

static void on_update(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    (void) length;
    cJSON *res = status == 200 ? cJSON_Parse(body) : NULL;
    if (cJSON_IsObject(res)) handle_poll(res);
    else cJSON_Delete(res);
}

static void copied_reset(void *user) {
    (void) user;
    g_just_copied = -2;
    menu_rerender_if_open(27);
}

int ranked_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    if (!name || !backend_enabled() || !acc()) return -1;
    char extra[256], v[160];
    if (!strcmp(name, "joinQueue")) join_queue(argc > 0 && args[0].kind == HTML_ARG_BOOL && args[0].num != 0);
    else if (!strcmp(name, "leaveQueue")) leave_queue();
    else if (!strcmp(name, "setPartyReady")) {
        if (g_party && g_client_key[0]) request("/mm/queue/update-ready", "&isReady=true", on_update);
        else join_queue(1);
    } else if (!strcmp(name, "setPartyUnready")) {
        if (g_party && g_client_key[0]) request("/mm/queue/update-ready", "&isReady=false", on_update);
    } else if (!strcmp(name, "setPartyQueues")) {
        if (!g_party || !g_client_key[0]) return 1;
        cJSON *ids = selected_queues();
        char *text = cJSON_PrintUnformatted(ids);
        cJSON_Delete(ids);
        enc(v, sizeof(v), text ? text : "[]");
        free(text);
        snprintf(extra, sizeof(extra), "&queues=%s", v);
        request("/mm/queue/update-queues", extra, on_update);
    } else if (!strcmp(name, "kickPartyMember") || !strcmp(name, "promotePartyMember")) {
        if (!g_party || argc < 1 || args[0].kind != HTML_ARG_STR) return 1;
        enc(v, sizeof(v), args[0].str);
        const int kick = name[0] == 'k';
        snprintf(extra, sizeof(extra), "&%s=%s", kick ? "kickClientId" : "promoteClientId", v);
        request(kick ? "/mm/queue/kick-party-member" : "/mm/queue/promote-party-member", extra, on_update);
    } else if (!strcmp(name, "copyPartyInviteLink")) {
        // UTILS.copyToClipboard("https://" + location.host + "/?party=" + partyId): no clipboard here, the
        // key is the party id the friend pastes into the Party Key/URL box
        g_just_copied = argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : -1;
        const char *pid = cJSON_GetStringValue(cJSON_GetObjectItem(g_party, "partyId"));
        if (pid) printf("[ranked] party invite: %s\n", pid);
        menu_rerender_if_open(27);
        html_timer_add(1250, copied_reset, NULL);
    } else {
        return -1;
    }
    return 1;
}

// selectClass while in a party: client.queueUpdateClass
void ranked_ui_class_changed(const int index) {
    if (!g_party || !g_client_key[0]) return;
    char extra[48];
    snprintf(extra, sizeof(extra), "&classIndex=%d", index);
    request("/mm/queue/update-class", extra, on_update);
}

// windows[26].gen() for an account
// windows[26] (showWindow(27)) for everyone: v7.1.4's gen() writes nothing (ranked play is
// gone there); KrunkNative keeps ranked: a guest gets the LOGIN REQUIRED card, an account the
// queue / party UI below, styled by the v3.1.1 rules in css/ranked-kn.css.
char *ranked_ui_window_gen(void) {
    static int css;
    if (!css) css = html_ui_add_stylesheet("css/ranked-kn.css") || 1;
    if (!account_get() || backend_available() != 1) {
        return strdup("<div class='reqW login' onclick='showWindow(5)'><i class='material-icons reqWIcon'>clear</i><div class='reqWLvl'>LOGIN REQUIRED</div><div class='reqWTxt'>to play in ranked games!</div></div>");
    }
    return ranked_window_gen();
}

char *ranked_window_gen(void) {
    const AccountInfo *a = acc();
    StrBuf sb = {0};
    if (g_cant_queue > 0) {
        const long long m = (long long) (g_cant_queue / 60000.0);
        sb_appendf(&sb, "<span style='font-size: 20px'>You have abandoned a ranked match in the last 5 hours. Please wait %s%lldh %lldm  to requeue.</span>", "", m / 60, m % 60);
        return sb_finish(&sb);
    }
    sb_append(&sb, "<div style='font-size:22px;margin-bottom:10px'>Ranked Queue<div style='display:inline-block;float:right;vertical-align:top;color:rgba(0,0,0,0.3)'><span>0</span> Valour</div></div><div class='rankedHeader'>");
    if (g_party) {
        sb_append(&sb, "<a onclick='leaveQueue()' onmouseenter='SOUND.play(`tick_0`,0.1)'>Leave Party</a> | ");
        sb_append(&sb, g_just_copied == -1 ? "<a>Copied</a>" : "<a onclick='copyPartyInviteLink(-1)' onmouseenter='SOUND.play(`tick_0`,0.1)'>Invite</a>");
    } else {
        sb_append(&sb, "\n                <input id='rankedPartyKey' placeholder='Party Key/URL' type='text'></input>\n                <span><a class='partyCreate' onclick='joinQueue()' onmouseenter='SOUND.play(\"tick_0\",0.1)'>Create Party</a>\n                <a class='partyJoin' onclick='joinQueue()' onmouseenter='SOUND.play(\"tick_0\",0.1)'>Join Party</a></span>");
    }
    sb_append(&sb, "</div>");
    if (g_party) {
        const char *region = cJSON_GetStringValue(cJSON_GetObjectItem(g_party, "region"));
        sb_append(&sb, "<div class='windowRightInfoText'>");
        sb_append_esc(&sb, cJSON_GetStringValue(cJSON_GetObjectItem(g_party, "partyId")) ? cJSON_GetStringValue(cJSON_GetObjectItem(g_party, "partyId")) : "");
        sb_append(&sb, " | ");
        sb_append_esc(&sb, region ? region : "");
        sb_append(&sb, "</div>");
    } else {
        char saved[32] = "";
        if (html_storage_get("krunker_rankedRegion", saved, sizeof(saved)) < 0 || !saved[0]) {
            if (html_storage_get("pingRegion7", saved, sizeof(saved)) < 0) saved[0] = 0;
        }
        // KrunkNative's regions (servers_ui.c: 2-letter codes, the place name as tooltip)
        char *options = servers_ui_region_options(saved);
        sb_append(&sb, "<select id='queueRegionSelect' class='inputGrey'>");
        if (options) sb_append(&sb, options);
        free(options);
        sb_append(&sb, "</select>");
    }
    const int leader = party_is_leader();
    const cJSON *members = cJSON_GetObjectItem(g_party, "partyMembers");
    const cJSON *qids = cJSON_GetObjectItem(g_party, "queueIds");
    const int member_count = cJSON_GetArraySize(members);
    if (g_party) {
        sb_append(&sb, "<div id='partyMemberList'>");
        const int slots = member_count > 4 ? member_count : 4;
        for (int i = 0; i < slots; i++) {
            const cJSON *m = cJSON_GetArrayItem(members, i);
            int locked = 0;
            if (!m) {
                for (int q = 0; q < QUEUE_COUNT; q++) {
                    const cJSON *id;
                    cJSON_ArrayForEach(id, qids) if (cJSON_IsString(id) && !strcmp(id->valuestring, k_queues[q].id) && i >= k_queues[q].max_party) locked = 1;
                }
            }
            const char *mid = cJSON_GetStringValue(cJSON_GetObjectItem(m, "clientId"));
            const char *leader_id = cJSON_GetStringValue(cJSON_GetObjectItem(g_party, "partyLeaderId"));
            const int is_leader = m && mid && leader_id && !strcmp(mid, leader_id);
            cJSON *data = m ? cJSON_Parse(cJSON_GetStringValue(cJSON_GetObjectItem(m, "accountData")) ? cJSON_GetStringValue(cJSON_GetObjectItem(m, "accountData")) : "{}") : NULL;
            const int ready = m && cJSON_IsTrue(cJSON_GetObjectItem(m, "isReady"));
            sb_appendf(&sb, "\n                    <div class='partyMember  %s %s'>\n                        <div class='partyAvatar %s'>\n                            <img src='./textures/classes/icon_%d.png'>\n                            ",
                       ready ? "ready" : "unready", m ? "" : "placeholder", is_leader ? "leader" : "",
                       m ? (int) cJSON_GetNumberValue(cJSON_GetObjectItem(m, "classIndex")) : i - 1);
            if (locked) sb_append(&sb, "<i class='material-icons partyMemberLock'>lock</i>");
            if (is_leader) sb_append(&sb, "<div class='partyLeaderIcon'><i class='material-icons'>star</i></div>");
            if (m && leader && !is_leader) {
                sb_append(&sb, "<div class='partyMemberActions'>\n                                        <a class='partyMemberAction' onclick='kickPartyMember(\"");
                sb_append_esc(&sb, mid ? mid : "");
                sb_append(&sb, "\")' onmouseenter='SOUND.play(\"tick_0\",0.1)'>Kick</a>\n                                        <div class='partyMemberActionSep'></div>\n                                        <a class='partyMemberAction' onclick='promotePartyMember(\"");
                sb_append_esc(&sb, mid ? mid : "");
                sb_append(&sb, "\")' onmouseenter='SOUND.play(\"tick_0\",0.1)'>Promote</a>\n                                    </div>\n                                ");
            }
            sb_append(&sb, "\n                        </div>\n                        <div class='partyUsername'>\n                            ");
            if (m) {
                const char *aid = cJSON_GetStringValue(cJSON_GetObjectItem(m, "accountId"));
                const char *clan = cJSON_GetStringValue(cJSON_GetObjectItem(data, "c"));
                sb_append(&sb, "<span>");
                sb_append_esc(&sb, aid ? aid : "");
                if (clan && clan[0]) {
                    sb_append(&sb, " <span style='color:rgba(0,0,0,0.7)'> [");
                    sb_append_esc(&sb, clan);
                    sb_append(&sb, "]</span>");
                }
                sb_append(&sb, "</span>");
            }
            sb_append(&sb, "\n                            ");
            if (!m && !locked && g_just_copied == i) sb_append(&sb, "<a>Copied</a>");
            else if (!m && !locked) sb_appendf(&sb, "<a onclick='copyPartyInviteLink(%d)' onmouseenter='SOUND.play(\"tick_0\",0.1)'>Invite</a>", i);
            if (locked) sb_append(&sb, "<span style='opacity: 0.75'>LOCKED</span>");
            if (m) {
                int id = (int) (cJSON_GetNumberValue(cJSON_GetObjectItem(data, "e")) / 120.0);
                id = (id > 8 ? 8 : id < 1 ? 1 : id) - 1;
                sb_appendf(&sb, "<img class='accountEloIcon' style='margin-top:2px' src='./img/ranks/icon_%d.png'>", id);
            }
            sb_append(&sb, "\n                        </div>\n                    </div>\n                ");
            cJSON_Delete(data);
        }
        sb_append(&sb, "</div>");
    }
    sb_appendf(&sb, "<div class='rankedQueueOptions' style='pointer-events: %s'>", leader ? "all" : "none");
    char saved_q[128] = "";
    if (html_storage_get("krk_lastQueues", saved_q, sizeof(saved_q)) < 0) saved_q[0] = 0;
    for (int q = 0; q < QUEUE_COUNT; q++) {
        const int too_large = g_party && member_count > k_queues[q].max_party;
        int checked = q == 1; // config.queues defaultQueue (r2v2)
        if (g_party) {
            checked = 0;
            const cJSON *id;
            cJSON_ArrayForEach(id, qids) if (cJSON_IsString(id) && !strcmp(id->valuestring, k_queues[q].id)) checked = 1;
        } else if (saved_q[0]) {
            checked = strstr(saved_q, k_queues[q].id) != NULL;
        }
        const double elo = a && a->data ? cJSON_GetNumberValue(cJSON_GetArrayItem(a->data, k_queues[q].elo_slot)) : 0;
        int rank = (int) (elo / 120.0);
        rank = (rank > 8 ? 8 : rank < 1 ? 1 : rank) - 1; // UTILS.eloIconId
        sb_appendf(&sb, "<label class='rankedSelection %s' onmouseenter='SOUND.play(`tick_0`,0.1)'>", too_large ? "tooLarge" : "");
        if (too_large) sb_append(&sb, "<i class='material-icons rankedLocked blackShad'>lock</i>");
        else {
            sb_appendf(&sb, "<div class='rankedType blackShad'>%s</div><img class='rankedELO' src='./img/ranks/icon_%d.png'><div class='rankedELOTxt blackShad'>", k_queues[q].name, rank);
            if (rank == 0) sb_append(&sb, "No Rank");
            else sb_appendf(&sb, "Rank %d", rank);
            sb_append(&sb, "</div>");
        }
        sb_appendf(&sb, "<input type='checkbox' class='rankedQueueOption' onchange='setPartyQueues()' data-queue-id='%s' %s %s><img class='rankedImg' src='/img/ranked/%s.png'></label>",
                   k_queues[q].id, checked && !too_large ? "checked" : "", too_large ? "true" : "false", k_queues[q].id);
    }
    sb_append(&sb, "</div>");
    sb_append(&sb, "<span class='rankedWarning'>");
    sb_append(&sb, g_status[0] ? g_status : "Leaving a match will result in a 2 Hour Ban &amp; loss in Rank");
    sb_append(&sb, "</span>");
    if (a && a->level < MIN_RANKED_LEVEL) {
        sb_appendf(&sb, "<div class=\"joinQueue jQR\">Requires lvl %d</div>", MIN_RANKED_LEVEL);
    } else if (g_client_key[0] && !g_party) {
        sb_appendf(&sb, "<div class=\"joinQueue jQR\">%s</div>", g_status);
    } else {
        int me_ready = 0;
        const cJSON *m;
        cJSON_ArrayForEach(m, members) {
            const char *mid = cJSON_GetStringValue(cJSON_GetObjectItem(m, "clientId"));
            if (mid && !strcmp(mid, g_client_id)) me_ready = cJSON_IsTrue(cJSON_GetObjectItem(m, "isReady"));
        }
        sb_appendf(&sb, "<div class=\"joinQueue%s\" %s onmouseenter='SOUND.play(`tick_0`,0.1)'>%s</div>", me_ready ? " jQR" : "",
                   me_ready ? "onclick=\"setPartyUnready()\"" : "onclick=\"setPartyReady()\"", me_ready ? "Unready" : "Ready");
    }
    return sb_finish(&sb);
}
