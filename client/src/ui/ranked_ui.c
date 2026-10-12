// Krunker v7.1.4 ranked (doc/v7/decisions.md "Ranked", doc/backend.md 6.5): the Ranked window
// (windows[26] = showWindow(27), opened from the main menu bar's Ranked button, menu_ui.c) and
// the queue session.
//
// v7.1.4 keeps the window (header "Ranked", label "ranked", dark, popup, width 1800, extraCls
// menuWindowF, maxH calc(100% - 325px), yPos 48, tabIndex / switchTab, justCopied, the region
// flagLinks), its strings (windows.ranked.login "to play in ranked games!", windows.ranked.level,
// ranked.join "Joined Ranked Match. Abandoning a match will result in a temporary ban") and the
// config (minRankedLevel 15, rankedCooldown, rankedDeduction, eloPer, eloK, rankedClasses), but
// its gen() returns "" in the reconstruction and its client has no queue code left. So the body
// is KrunkNative's, in v7's window vocabulary (bigMenTabs / bigMenTab tabs, setBodH / settName
// rows, reqW cards, joinBtn): tab 0 Play (region, party, ready / find match), tab 1 the season's
// rank card (/mm/queue/rating). A match is v7's "Krunker Competitive" 4v4 Hardpoint comp lobby
// (comp_ui.c). The queue routes are the brain's /mm/queue/* (v3.1.1 ClientManager shapes):
// /mm/queue/join, /mm/queue/poll every 1 s, update-ready / update-class / kick / promote /
// leave; a formed match ("Join" {gameId, host, port, webUrl, clientId}) is joined with its ticket.
#include <backend.h>
#include <backend_ui.h>
#include <ui_logic.h>
#include <html_ui.h>
#include <ranked.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JOIN_COUNTDOWN 5    // joinCountdownSeconds
#define RATING_TTL_MS 30000 // the rank card is refetched when the window opens after this
#define PARTY_SLOTS RANKED_TEAM_SIZE

static char g_client_id[64], g_client_key[128]; // client.queueManager (empty = not queued)
static cJSON *g_party;            // party: the last poll response (owned)
static char g_status[96];         // queueStatusMessage ("" = null)
static char g_ready[32];          // queueReadyMessage
static int g_poll_timer, g_countdown_timer, g_countdown, g_countdown_started;
static double g_cant_queue;       // the RankedCooldown left (ms), 0 = none
static int g_just_copied = -2;    // windows[26].justCopied (-2 = null)
static int g_tab;                 // windows[26].tabIndex
static char g_new_game[160];      // QueueManager.newGameId
static cJSON *g_rating;           // GET /mm/queue/rating (owned), NULL = not loaded
static int g_rating_fresh, g_rating_inflight;
static int g_in_match;            // joined a ranked match (v7 config.isComp: no hosting)
static int g_scenario;            // --render-scenario: state.ranked stands in for the brain

static const AccountInfo *acc(void) { return account_get(); }

static void enc(char *out, size_t n, const char *s) { backend_url_encode(out, n, s ? s : ""); }

static int party_is_leader(void) {
    const char *leader = cJSON_GetStringValue(cJSON_GetObjectItem(g_party, "partyLeaderId"));
    return !g_party || (g_client_id[0] && leader && !strcmp(leader, g_client_id));
}

static int min_level(void) {
    const cJSON *v = cJSON_GetObjectItem(g_rating, "minLevel");
    return cJSON_IsNumber(v) ? v->valueint : RANKED_MIN_LEVEL;
}

static int team_size(void) {
    const cJSON *v = cJSON_GetObjectItem(g_rating, "teamSize");
    return cJSON_IsNumber(v) && v->valueint > 0 && v->valueint <= RANKED_TEAM_SIZE ? v->valueint : RANKED_TEAM_SIZE;
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
            const cJSON *members = cJSON_GetObjectItem(g_party, "partyMembers");
            int ready = 0, total = 0;
            const cJSON *m;
            cJSON_ArrayForEach(m, members) {
                total++;
                if (cJSON_IsTrue(cJSON_GetObjectItem(m, "isReady"))) ready++;
            }
            if (!t) t = "";
            if (!strcmp(t, "Queued")) {
                if (ready == total) snprintf(g_status, sizeof(g_status), "Finding match [%d in queue]", (int) cJSON_GetNumberValue(cJSON_GetObjectItem(c, "queueSize")));
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
                snprintf(g_status, sizeof(g_status), "Queuing");
            }
        }
        if (g_party) {
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

static void update_status_cb(void *user) {
    (void) user;
    update_status(NULL);
}

const char *ranked_ui_status(void) { return g_status[0] ? g_status : NULL; }
const char *ranked_ui_ready(void) { return g_ready[0] ? g_ready : NULL; }
int ranked_ui_in_ranked(void) { return g_client_key[0] || g_in_match; }

void ranked_ui_left_match(void) { g_in_match = 0; }

// ------------------------------------------------------------------ the rank card

static void rating_expire(void *user) {
    (void) user;
    g_rating_fresh = 0;
}

static void on_rating(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    (void) length;
    g_rating_inflight = 0;
    cJSON *res = status == 200 ? cJSON_Parse(body) : NULL;
    if (!cJSON_IsObject(res)) {
        cJSON_Delete(res);
        return;
    }
    cJSON_Delete(g_rating);
    g_rating = res;
    g_rating_fresh = 1;
    html_timer_add(RATING_TTL_MS, rating_expire, NULL);
    const cJSON *cd = cJSON_GetObjectItem(res, "cooldownMs");
    g_cant_queue = cJSON_IsNumber(cd) ? cd->valuedouble : 0.0;
    menu_rerender_if_open(27);
}

static void fetch_rating(void) {
    if (g_scenario || g_rating_inflight || g_rating_fresh || !acc() || backend_available() != 1) return;
    char token[256], path[400];
    enc(token, sizeof(token), account_session_token());
    snprintf(path, sizeof(path), "/mm/queue/rating?token=%s", token);
    g_rating_inflight = 1;
    if (backend_http_get(path, on_rating, NULL) != 0) g_rating_inflight = 0;
}

// ------------------------------------------------------------------ queue session

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
            // joinCallback(gameId): unqueue + switchGame with the match ticket
            const char *host = cJSON_GetStringValue(cJSON_GetObjectItem(c, "host"));
            const int port = (int) cJSON_GetNumberValue(cJSON_GetObjectItem(c, "port"));
            const char *ticket = cJSON_GetStringValue(cJSON_GetObjectItem(c, "clientId"));
            const char *web = cJSON_GetStringValue(cJSON_GetObjectItem(c, "webUrl"));
            printf("[ranked] match ready: joining %s\n", game);
            request("/mm/queue/leave", "", NULL);
            unqueue();
            cJSON_Delete(g_party);
            g_party = NULL;
            g_in_match = 1;
            g_rating_fresh = 0; // the match changes the rating
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
        } else {
            const char *text = !st ? "Failed"
                             : !strcmp(st, "AccountAlreadyPlaying") ? "Already queued in another tab"
                             : !strcmp(st, "LevelTooLow") ? "Level too low"
                             : !strcmp(st, "LoginRequired") ? "Login to play in ranked games"
                             : st;
            update_status(text);
            html_timer_add(2000, update_status_cb, NULL);
        }
    }
    cJSON_Delete(res);
}

static int class_for_queue(void) {
    const int index = menu_ui()->client->menu_class_index;
    return ranked_class_allowed(index) ? index : g_ranked_classes[0];
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
    char e_token[256], e_region[96], e_party[256], path[1400];
    enc(e_token, sizeof(e_token), account_session_token());
    enc(e_region, sizeof(e_region), region);
    enc(e_party, sizeof(e_party), party);
    snprintf(path, sizeof(path), "/mm/queue/join?token=%s&region=%s&party=%s&ready=%d&classIndex=%d", e_token, e_region, e_party, ready,
             class_for_queue());
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

// "https://" + location.host + "/?party=" + partyId
static void copy_party_link(void) {
    const char *pid = cJSON_GetStringValue(cJSON_GetObjectItem(g_party, "partyId"));
    if (!pid) return;
    char link[512];
    servers_ui_invite_link(link, sizeof(link));
    char *q = strstr(link, "/?");
    if (q) q[1] = 0; // the page URL without its ?game=
    else snprintf(link, sizeof(link), "/");
    const size_t len = strlen(link);
    snprintf(link + len, sizeof(link) - len, "?party=%s", pid);
    html_clipboard_write(link);
}

int ranked_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    if (!name) return -1;
    if (!strcmp(name, "windows.26.switchTab")) { // windows[26].switchTab(index)
        const int tab = argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : 0;
        if (tab != g_tab) {
            g_tab = tab;
            menu_rerender_if_open(27);
        }
        return 1;
    }
    if (!strcmp(name, "closeRankedPopup")) { // window.closeRankedPopup = clearPops(true)
        menu_show_window(MENU_WINDOW_NONE, 0);
        return 1;
    }
    if (!backend_enabled() || !acc()) return -1;
    char extra[256], v[160];
    if (!strcmp(name, "joinQueue")) join_queue(argc > 0 && args[0].kind == HTML_ARG_BOOL && args[0].num != 0);
    else if (!strcmp(name, "leaveQueue")) leave_queue();
    else if (!strcmp(name, "setPartyReady")) {
        if (g_party && g_client_key[0]) request("/mm/queue/update-ready", "&isReady=true", on_update);
        else join_queue(1);
    } else if (!strcmp(name, "setPartyUnready")) {
        if (g_party && g_client_key[0]) request("/mm/queue/update-ready", "&isReady=false", on_update);
    } else if (!strcmp(name, "kickPartyMember") || !strcmp(name, "promotePartyMember")) {
        if (!g_party || argc < 1 || args[0].kind != HTML_ARG_STR) return 1;
        enc(v, sizeof(v), args[0].str);
        const int kick = name[0] == 'k';
        snprintf(extra, sizeof(extra), "&%s=%s", kick ? "kickClientId" : "promoteClientId", v);
        request(kick ? "/mm/queue/kick-party-member" : "/mm/queue/promote-party-member", extra, on_update);
    } else if (!strcmp(name, "copyPartyInviteLink")) { // windows[26].justCopied
        g_just_copied = argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : -1;
        copy_party_link();
        menu_rerender_if_open(27);
        html_timer_add(1250, copied_reset, NULL);
    } else {
        return -1;
    }
    return 1;
}

// selectClass while in a party: client.queueUpdateClass (rankedClasses only)
void ranked_ui_class_changed(const int index) {
    if (!g_party || !g_client_key[0] || !ranked_class_allowed(index)) return;
    char extra[48];
    snprintf(extra, sizeof(extra), "&classIndex=%d", index);
    request("/mm/queue/update-class", extra, on_update);
}

// ------------------------------------------------------------------ windows[26].gen()

static void gen_tabs(StrBuf *sb) {
    const cJSON *season = cJSON_GetObjectItem(g_rating, "season");
    char label[32];
    if (cJSON_IsNumber(season)) snprintf(label, sizeof(label), "Season %d", season->valueint);
    else snprintf(label, sizeof(label), "Season");
    const char *tabs[2] = {"Play", label};
    sb_append(sb, "<div style='height:15px;'></div><div class='bigMenTabs'>");
    for (int i = 0; i < 2; i++) {
        sb_appendf(sb, "<div class='bigMenTab%s' onclick='playSelect(0.1);windows[26].switchTab(%d)' onmouseenter='playTick();' "
                       "style='width:50%%;padding-top:10px;padding-bottom:5px;'>%s</div>", g_tab == i ? " bigMenTabA" : "", i, tabs[i]);
    }
    sb_append(sb, "</div>");
}

static int rating_value(const char *key) {
    const cJSON *v = cJSON_GetObjectItem(g_rating, key);
    return cJSON_IsNumber(v) ? v->valueint : 0;
}

// the rank chip: UTILS.eloIconHTML (only icon_0 survives in Krunker's assets) + "Rank N"
static void rank_chip(StrBuf *sb, const double rating) {
    sb_appendf(sb, "<span class='rkdRank'><img class='accountEloIcon' src='./img/ranks/icon_0.png'> Rank %d <span class='rkdElo'>%d</span></span>",
               ranked_rank_number(rating), (int) rating);
}

static void gen_season(StrBuf *sb) {
    sb_append(sb, "<div class='setBodH'>");
    if (!g_rating) {
        sb_append(sb, "<div class='settName'>Loading...</div></div>");
        return;
    }
    const int rating = rating_value("rating");
    sb_append(sb, "<div class='settName'>Rank<span class='rkdVal'>");
    rank_chip(sb, rating);
    sb_append(sb, "</span></div>");
    const char *rows[][2] = {{"Peak Rating", "peak"}, {"Games", "games"}, {"Wins", "wins"}, {"Losses", "losses"}, {"Abandons", "abandons"}};
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        sb_appendf(sb, "<div class='settName'>%s<span class='rkdVal'>%d</span></div>", rows[i][0], rating_value(rows[i][1]));
    }
    sb_appendf(sb, "<div class='settName'>A new rank every %d rating. Winners gain and losers lose up to %d against the other team's "
                   "average; abandoning costs %d and a %d hour queue ban.</div>",
               RANKED_ELO_PER, (int) RANKED_ELO_K, RANKED_DEDUCTION, RANKED_COOLDOWN_MS / 3600000);
    sb_append(sb, "</div>");
}

static void gen_party(StrBuf *sb) {
    const int leader = party_is_leader();
    const cJSON *members = cJSON_GetObjectItem(g_party, "partyMembers");
    const char *leader_id = cJSON_GetStringValue(cJSON_GetObjectItem(g_party, "partyLeaderId"));
    sb_append(sb, "<div class='rkdSlots'>");
    for (int i = 0; i < team_size(); i++) {
        const cJSON *m = cJSON_GetArrayItem(members, i);
        const char *mid = cJSON_GetStringValue(cJSON_GetObjectItem(m, "clientId"));
        const int is_leader = m && mid && leader_id && !strcmp(mid, leader_id);
        const int ready = m && cJSON_IsTrue(cJSON_GetObjectItem(m, "isReady"));
        const char *data_s = cJSON_GetStringValue(cJSON_GetObjectItem(m, "accountData"));
        cJSON *data = data_s ? cJSON_Parse(data_s) : NULL;
        const int cls = m ? (int) cJSON_GetNumberValue(cJSON_GetObjectItem(m, "classIndex")) : -1;
        sb_appendf(sb, "<div class='rkdSlot%s'><img class='rkdSlotIcn' src='./textures/classes/icon_%d.png'>", m ? (ready ? " ready" : " unready") : " open", cls);
        if (is_leader) sb_append(sb, "<i class='material-icons rkdLead'>star</i>");
        sb_append(sb, "<div class='rkdSlotName'>");
        if (m) {
            const char *aid = cJSON_GetStringValue(cJSON_GetObjectItem(m, "accountId"));
            const char *clan = cJSON_GetStringValue(cJSON_GetObjectItem(data, "c"));
            sb_append_esc(sb, aid ? aid : "");
            if (clan && clan[0]) {
                sb_append(sb, " <span style='color:rgba(255,255,255,0.5)'>[");
                sb_append_esc(sb, clan);
                sb_append(sb, "]</span>");
            }
        } else {
            sb_append(sb, "Open Slot");
        }
        sb_append(sb, "</div><div class='rkdSlotSub'>");
        if (m) {
            rank_chip(sb, cJSON_GetNumberValue(cJSON_GetObjectItem(data, "e")));
            sb_appendf(sb, " <span class='%s'>%s</span>", ready ? "rkdReady" : "rkdUnready", ready ? "Ready" : "Not Ready");
            if (leader && !is_leader && mid) {
                sb_append(sb, "<div><a class='menuLink' onclick='kickPartyMember(\"");
                sb_append_esc(sb, mid);
                sb_append(sb, "\")' onmouseenter='playTick()'>Kick</a> | <a class='menuLink' onclick='promotePartyMember(\"");
                sb_append_esc(sb, mid);
                sb_append(sb, "\")' onmouseenter='playTick()'>Promote</a></div>");
            }
        } else if (g_party && g_just_copied == i) {
            sb_append(sb, "Copied");
        } else if (g_party) {
            sb_appendf(sb, "<a class='menuLink' onclick='copyPartyInviteLink(%d)' onmouseenter='playTick()'>Invite</a>", i);
        }
        sb_append(sb, "</div></div>");
        cJSON_Delete(data);
    }
    sb_append(sb, "</div>");
}

static void gen_play(StrBuf *sb) {
    const AccountInfo *a = acc();
    // the match: config.hostTypes[1] "Krunker Competitive" (index.js cmpDatBox / cmpDatSub)
    sb_appendf(sb, "<div class='rkdHead'><div class='rkdTitle'>Krunker Competitive<div class='rkdSub'>%dv%d <span style='color:rgba(255,255,255,0.5)'>"
                   "on Hardpoint</span></div></div>", team_size(), team_size());
    if (g_rating) rank_chip(sb, rating_value("rating"));
    sb_append(sb, "</div>");
    if (g_cant_queue > 0) {
        const long long m = (long long) (g_cant_queue / 60000.0);
        sb_appendf(sb, "<div class='setBodH'><div class='settName'>You abandoned a ranked match. Please wait %lldh %lldm to requeue.</div></div>", m / 60, m % 60);
        return;
    }
    sb_append(sb, "<div class='setBodH'>");
    // Region
    sb_append(sb, "<div class='settName'>Region");
    if (g_party) {
        const char *region = cJSON_GetStringValue(cJSON_GetObjectItem(g_party, "region"));
        sb_append(sb, "<span class='rkdVal'>");
        sb_append_esc(sb, region ? region : "");
        sb_append(sb, "</span>");
    } else {
        char saved[32] = "";
        if (html_storage_get("krunker_rankedRegion", saved, sizeof(saved)) < 0 || !saved[0]) {
            if (html_storage_get("pingRegion7", saved, sizeof(saved)) < 0) saved[0] = 0;
        }
        char *options = servers_ui_region_options(saved);
        sb_append(sb, "<select id='queueRegionSelect' class='inputGrey'>");
        if (options) sb_append(sb, options);
        free(options);
        sb_append(sb, "</select>");
    }
    sb_append(sb, "</div>");
    // Party
    sb_append(sb, "<div class='settName'>Party");
    if (g_party) {
        const char *pid = cJSON_GetStringValue(cJSON_GetObjectItem(g_party, "partyId"));
        sb_append(sb, "<span class='rkdVal'>");
        sb_append_esc(sb, pid ? pid : "");
        sb_append(sb, " | ");
        sb_append(sb, g_just_copied == -1 ? "Copied" : "<a class='menuLink' onclick='copyPartyInviteLink(-1)' onmouseenter='playTick()'>Invite</a>");
        sb_append(sb, " | <a class='menuLink' onclick='leaveQueue()' onmouseenter='playTick()'>Leave Party</a></span>");
    } else if (!g_client_key[0]) {
        sb_append(sb, "<span class='rkdVal'><input id='rankedPartyKey' class='inputGrey' placeholder='Party Key/URL' type='text'><span>"
                      "<a class='menuLink partyCreate' onclick='joinQueue()' onmouseenter='playTick()'>Create Party</a>"
                      "<a class='menuLink partyJoin' onclick='joinQueue()' onmouseenter='playTick()'>Join Party</a></span></span>");
    }
    sb_append(sb, "</div></div>");
    if (g_party) gen_party(sb);
    // ranked.join: the abandon rule
    sb_appendf(sb, "<div class='rkdWarn'>%s</div>", g_status[0] ? g_status : "Abandoning a match will result in a temporary ban &amp; loss in rank");
    if (a && a->level < min_level()) return;
    int me_ready = 0;
    const cJSON *m;
    cJSON_ArrayForEach(m, cJSON_GetObjectItem(g_party, "partyMembers")) {
        const char *mid = cJSON_GetStringValue(cJSON_GetObjectItem(m, "clientId"));
        if (mid && !strcmp(mid, g_client_id)) me_ready = cJSON_IsTrue(cJSON_GetObjectItem(m, "isReady"));
    }
    if (g_client_key[0] && !g_party) {
        sb_appendf(sb, "<div class='joinBtn rkdGo rkdBusy'>%s</div>", g_status);
    } else if (me_ready) {
        sb_append(sb, "<div class='joinBtn rkdGo rkdBusy' onclick='setPartyUnready()' onmouseenter='playTick()'>Cancel</div>");
    } else {
        sb_appendf(sb, "<div class='joinBtn rkdGo' onclick='setPartyReady()' onmouseenter='playTick()'>%s</div>", g_party ? "Ready" : "Find Match");
    }
}

// windows[26] (showWindow(27)) for everyone: a guest gets the LOGIN REQUIRED card
// (windows.ranked.login), a low level the LEVEL REQUIRED card (windows.ranked.level, without its
// <span color="black"> wrappers: an attribute browsers ignore).
char *ranked_ui_window_gen(void) {
    static int css;
    if (!css) css = html_ui_add_stylesheet("css/ranked-kn.css") || 1;
    const AccountInfo *a = acc();
    if (!a || (backend_available() != 1 && !g_scenario)) {
        return strdup("<div class='reqW login' onclick='showWindow(5)'><i class='material-icons reqWIcon'>clear</i><div class='reqWLvl'>LOGIN REQUIRED</div><div class='reqWTxt'>to play in ranked games!</div></div>");
    }
    fetch_rating();
    if (a->level < min_level()) {
        StrBuf sb = {0};
        sb_appendf(&sb, "<div class='reqW'><i class='material-icons reqWIcon'>clear</i><div class='reqWLvl'>LEVEL %d REQUIRED</div><div class='reqWTxt'>"
                        "You must be at least level %d to play in ranked games! Right now you are at level %d.</div></div>", min_level(), min_level(), a->level);
        return sb_finish(&sb);
    }
    StrBuf sb = {0};
    gen_tabs(&sb);
    if (g_tab == 1) gen_season(&sb);
    else gen_play(&sb);
    return sb_finish(&sb);
}

void ranked_ui_scenario(const cJSON *state) {
    if (!cJSON_IsObject(state)) return;
    g_scenario = 1;
    cJSON_Delete(g_rating);
    const cJSON *rating = cJSON_GetObjectItem(state, "rating");
    g_rating = cJSON_IsObject(rating) ? cJSON_Duplicate(rating, 1) : NULL;
    cJSON_Delete(g_party);
    const cJSON *party = cJSON_GetObjectItem(state, "party");
    g_party = cJSON_IsObject(party) ? cJSON_Duplicate(party, 1) : NULL;
    const char *me = cJSON_GetStringValue(cJSON_GetObjectItem(state, "clientId"));
    snprintf(g_client_id, sizeof(g_client_id), "%s", me ? me : "");
    const cJSON *tab = cJSON_GetObjectItem(state, "tab");
    g_tab = cJSON_IsNumber(tab) ? tab->valueint : 0;
    const cJSON *cd = cJSON_GetObjectItem(state, "cooldownMs");
    g_cant_queue = cJSON_IsNumber(cd) ? cd->valuedouble : 0.0;
}
