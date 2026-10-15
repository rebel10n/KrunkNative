// The player list's actions (server_votekick.h): Krunker v10.0.2's vote kick and the K.P.D
// call's lobby rules. v10's own server is closed; the rules follow its client (game.deob.js:
// the Votekick button :214300, kickVote :220193, the [1]/[2] keys :221373, io "vk" :219974) and
// its config constants (:20095-20098), the rest follows v10's client (game.deob.js).
//
// A vote: NetVoteKickPacket {target alias, vote 1} from an account opens it (accounts only, like
// v10's button; never in ranked). The target is a connection (never a bot), not the voter, not
// DEV / op / host / moderator, below voteKickMaxLvl (20). Everyone gets "vk" (opened = 1), the
// initiator's yes counts, the others answer with [1] / [2] once each. votes_req = min(voteKickReq,
// eligible) where eligible = the other accounts in the lobby (at least 2). Enough yes votes: the
// target is kicked ("Vote Kicked", 004002) and its account (a guest: its address) is refused for
// voteKickBan (30 min); too many no votes or the 35 s timer (game.update, hooks.vote_kick_expired):
// the vote failed. One vote at a time, and none for 35 s after the last one ended.
#include <server_votekick.h>
#include <server_accounts.h>
#include <server_log.h>
#include <shared.h>
#include <game_config.h> // game_config_is_custom: no vote kicks in hosted lobbies (v10 isCustom)
#include <chat_commands.h> // chat_command_admin_action: the DEV kick / ban (NET_EXT_ADMIN_ACTION)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define VK_BANS_MAX 128
#define KPD_RECENT_MAX 64
#define KPD_OFFICERS_MAX 16

typedef struct {
    int account_id;
    char address[CHAT_CMD_ADDRESS_SIZE];
    char name[SERVER_PLAYER_NAME_MAX];
    float until; // g_server->now
} VoteKickBan;

typedef struct {
    int account_id;    // the suspect (0: by name)
    char name[SERVER_PLAYER_NAME_MAX];
    float at;          // the last call about them
    int suspect_id;    // the suspect's connection (= player uid) at the call
    int caller_id;     // the reporter's
    int64_t call_id;   // kn_brain's kpd_calls.id (0: no coordinator, or not answered yet)
    int rated;         // an officer gave the verdict ("rkpd")
} KpdRecent;

// A K.P.D officer in this lobby with a case (v10 startGame kpdData {sus, caller})
typedef struct {
    int conn_id;
    int recent;        // index into g_vk.recent
    float since;       // the case was handed over (kpdWatchTime counts from here)
    int rated;
} KpdOfficer;

static struct {
    VoteKickBan bans[VK_BANS_MAX];
    int ban_count;
    float cooldown_until;   // no new vote before this (g_server->now)
    float last_kpd_call_at; // the lobby's last call (kpdCallTime)
    KpdRecent recent[KPD_RECENT_MAX];
    int recent_count;
    KpdOfficer officers[KPD_OFFICERS_MAX];
    int officer_count;
} g_vk;

// ---------------------------------------------------------------- helpers

static void vk_chat_to(const ClientConnection *client, const char *text) {
    NetChatPacket packet;
    net_packet_chat(&packet, -1, NET_CHAT_SERVER, NULL, text);
    net_send_packet(client->fd, NET_PACKET_CHAT, &packet, sizeof(packet));
}

static void vk_send_ext(const ClientConnection *to, const uint16_t subtype, const void *body, const uint16_t length) {
    if (!to || to->peer_minor < NET_PROTOCOL_MINOR_EXT) return;
    uint8_t buf[NET_EXT_HEADER_SIZE + NET_EXT_BODY_MAX];
    const uint16_t n = net_ext_pack(buf, sizeof(buf), subtype, body, length);
    if (n) net_send_packet(to->fd, NET_PACKET_EXT, buf, n);
}

static void vk_broadcast_ext(const uint16_t subtype, const void *body, const uint16_t length) {
    for (size_t i = 0; i < g_server->client_count; i++) vk_send_ext(&g_server->clients[i], subtype, body, length);
}

static int vk_admitted(const ClientConnection *client) {
    return client && client->acct.join_state == SERVER_JOIN_READY;
}

// v10's Votekick / K.P.D buttons are an account's (level >= 1): guests have neither
static int vk_is_account(const ClientConnection *client) {
    return vk_admitted(client) && client->acct.account_id > 0;
}

static int vk_protected(const ClientConnection *client) {
    return net_client_is_dev(client) || accounts_is_custom_host(client) || (client->acct.flags & NET_PLAYER_INFO_MOD) != 0;
}

static ClientConnection *vk_by_alias(const char *alias) {
    if (!alias || !alias[0]) return NULL;
    for (size_t i = 0; i < g_server->client_count; i++) {
        ClientConnection *c = &g_server->clients[i];
        if (vk_admitted(c) && !strcmp(c->name, alias)) return c;
    }
    for (size_t i = 0; i < g_server->client_count; i++) {
        ClientConnection *c = &g_server->clients[i];
        if (vk_admitted(c) && !strcasecmp(c->name, alias)) return c;
    }
    return NULL;
}

static ClientConnection *vk_by_id(const int id) {
    for (size_t i = 0; i < g_server->client_count; i++) {
        if (g_server->clients[i].id == id) return &g_server->clients[i];
    }
    return NULL;
}

static int vk_eligible(const int target_id) {
    int n = 0;
    for (size_t i = 0; i < g_server->client_count; i++) {
        const ClientConnection *c = &g_server->clients[i];
        n += vk_is_account(c) && c->id != target_id;
    }
    return n;
}

static int vk_voted(const VoteKick *vote, const int id) {
    for (int i = 0; i < vote->voter_count; i++) {
        if (vote->voters[i] == id) return 1;
    }
    return 0;
}

static void vk_state(NetVoteKickStatePacket *out, const VoteKick *vote, const int active, const int opened, const int passed) {
    memset(out, 0, sizeof(*out));
    out->active = (uint8_t) active;
    out->opened = (uint8_t) opened;
    out->passed = (uint8_t) passed;
    if (vote) {
        out->votes = vote->votes_for;
        out->needed = vote->votes_req;
        snprintf(out->target, sizeof(out->target), "%s", vote->target);
        snprintf(out->voter, sizeof(out->voter), "%s", vote->initiator);
    }
}

static void vk_broadcast(const VoteKick *vote, const int active, const int opened, const int passed) {
    NetVoteKickStatePacket state;
    vk_state(&state, vote, active, opened, passed);
    vk_broadcast_ext(NET_EXT_VOTE_KICK_STATE, &state, sizeof(state));
}

static void vk_end(Game *game, const int passed) {
    VoteKick *vote = game->vote_kick;
    if (!vote) return;
    vk_broadcast(vote, 0, 0, passed); // "vk" null: the prompts hide
    g_vk.cooldown_until = g_server->now + VOTE_KICK_COOLDOWN_S;
    free(vote);
    game->vote_kick = NULL;
}

static void vk_ban(const ClientConnection *target) {
    if (target->acct.account_id <= 0 && !target->address[0]) { // a guest with no address (the offline sandbox): nothing to hold
        server_log("[votekick] %s is a guest without an address: kicked, not banned", target->name);
        return;
    }
    VoteKickBan *b = NULL;
    for (int i = 0; i < g_vk.ban_count; i++) { // the same identity again: refreshed
        VoteKickBan *e = &g_vk.bans[i];
        if ((target->acct.account_id > 0 && e->account_id == target->acct.account_id) ||
            (target->acct.account_id <= 0 && !e->account_id && !strcmp(e->address, target->address))) b = e;
    }
    if (!b) {
        if (g_vk.ban_count < VK_BANS_MAX) b = &g_vk.bans[g_vk.ban_count++];
        else { // full: the one that expires first goes
            b = &g_vk.bans[0];
            for (int i = 1; i < g_vk.ban_count; i++) {
                if (g_vk.bans[i].until < b->until) b = &g_vk.bans[i];
            }
        }
    }
    memset(b, 0, sizeof(*b));
    b->account_id = target->acct.account_id > 0 ? target->acct.account_id : 0;
    snprintf(b->address, sizeof(b->address), "%s", target->address);
    snprintf(b->name, sizeof(b->name), "%s", target->name);
    b->until = g_server->now + VOTE_KICK_BAN_S;
}

static void vk_expired(Game *game) {
    (void) game;
    if (!game->vote_kick) return;
    server_log("[votekick] the vote against %s timed out (%d/%d)", game->vote_kick->target, game->vote_kick->votes_for,
               game->vote_kick->votes_req);
    net_broadcast_chat(NULL, "Vote kick failed");
    vk_broadcast(game->vote_kick, 0, 0, 0);
    g_vk.cooldown_until = g_server->now + VOTE_KICK_COOLDOWN_S;
    // game.c frees it right after this hook
}

// ---------------------------------------------------------------- public

void votekick_init(Game *game) {
    memset(&g_vk, 0, sizeof(g_vk));
    if (game) game->hooks.vote_kick_expired = vk_expired;
}

int votekick_banned(const int account_id, const char *address, char *message, const size_t size) {
    for (int i = 0; i < g_vk.ban_count;) {
        VoteKickBan *b = &g_vk.bans[i];
        if (b->until <= g_server->now) { // expired: dropped
            g_vk.bans[i] = g_vk.bans[--g_vk.ban_count];
            continue;
        }
        const int hit = account_id > 0 ? b->account_id == account_id : !b->account_id && address && address[0] && !strcmp(b->address, address);
        if (hit) {
            const int left = (int) ((b->until - g_server->now + 59.0f) / 60.0f);
            if (message && size) snprintf(message, size, "Vote kicked: you can rejoin in %d min", left > 0 ? left : 1);
            return 1;
        }
        i++;
    }
    return 0;
}

void votekick_send_state(const ClientConnection *to) {
    const VoteKick *vote = g_server->game.vote_kick;
    if (!vote || !to) return;
    NetVoteKickStatePacket state;
    vk_state(&state, vote, 1, 1, 0);
    vk_send_ext(to, NET_EXT_VOTE_KICK_STATE, &state, sizeof(state));
}

void votekick_on_leave(const ClientConnection *client) {
    Game *game = &g_server->game;
    for (int i = 0; client && i < g_vk.officer_count;) { // an officer left: the case is free again
        if (g_vk.officers[i].conn_id == client->id) g_vk.officers[i] = g_vk.officers[--g_vk.officer_count];
        else i++;
    }
    VoteKick *vote = game->vote_kick;
    if (!vote || !client) return;
    if (client->id == vote->target_uid) {
        server_log("[votekick] %s left during the vote against them", client->name);
        vk_end(game, 0);
        return;
    }
    if (vk_is_account(client) && !vk_voted(vote, client->id)) {
        vote->eligible = vote->eligible > 1 ? vote->eligible - 1 : 1; // one fewer possible vote
        if (vote->votes_req > vote->eligible) vote->votes_req = vote->eligible;
        if (vote->votes_for >= vote->votes_req) { // enough now
            ClientConnection *target = vk_by_id(vote->target_uid);
            if (target) {
                vk_ban(target);
                net_kick_client(target->id, "Vote Kicked", "004002");
            }
            vk_end(game, 1);
        }
    }
}

void votekick_handle(ClientConnection *client, const uint8_t *body, const uint16_t length) {
    Game *game = &g_server->game;
    if (!client || length < sizeof(NetVoteKickPacket)) return;
    NetVoteKickPacket req;
    memcpy(&req, body, sizeof(req));
    req.target[sizeof(req.target) - 1] = 0;
    if (!vk_is_account(client)) { // v10: the button and the keys are an account's
        vk_chat_to(client, "Sign in to vote kick");
        return;
    }
    if (game->is_ranked || accounts_ranked_active()) {
        vk_chat_to(client, "No vote kicks in ranked matches");
        return;
    }
    if (game_config_is_custom(game) && !game->is_sandbox) { // v10 !game.isCustom: a hosted lobby's host kicks (the sandbox votes)
        vk_chat_to(client, "No vote kicks in custom games");
        return;
    }
    VoteKick *vote = game->vote_kick;
    if (vote) { // a vote on the running one (yes or no; the initiator already voted)
        if (strcasecmp(req.target, vote->target) != 0) return; // for another player: v10 ignores it
        if (vk_voted(vote, client->id) || client->id == vote->target_uid) return;
        if (vote->voter_count >= VOTE_KICK_VOTERS_MAX) return;
        vote->voters[vote->voter_count++] = client->id;
        if (req.vote) vote->votes_for++;
        else vote->votes_no++;
        char text[NET_CHAT_TEXT_SIZE];
        snprintf(text, sizeof(text), "Vote Submitted - %d/%d", vote->votes_for, vote->votes_req);
        vk_chat_to(client, text); // v10's updateGameMessage(-1, null, "Vote Submitted - n/m", 2)
        if (vote->votes_for >= vote->votes_req) {
            ClientConnection *target = vk_by_id(vote->target_uid);
            server_log("[votekick] %s was vote kicked (%d/%d)", vote->target, vote->votes_for, vote->votes_req);
            if (target) {
                vk_ban(target);
                net_kick_client(target->id, "Vote Kicked", "004002");
            }
            snprintf(text, sizeof(text), "%s was vote kicked", vote->target);
            net_broadcast_chat(NULL, text);
            vk_end(game, 1);
            return;
        }
        if (vote->votes_for + (vote->eligible - vote->voter_count) < vote->votes_req) { // cannot pass any more
            server_log("[votekick] the vote against %s failed (%d yes, %d no)", vote->target, vote->votes_for, vote->votes_no);
            net_broadcast_chat(NULL, "Vote kick failed");
            vk_end(game, 0);
            return;
        }
        vk_broadcast(vote, 1, 0, 0); // "vk" target, votes, needed, false
        return;
    }
    if (!req.vote) return; // a "no" with no vote running
    if (g_vk.cooldown_until > g_server->now) {
        vk_chat_to(client, "A vote kick just ended, wait a moment");
        return;
    }
    ClientConnection *target = vk_by_alias(req.target);
    if (!target || target == client || !target->player) {
        vk_chat_to(client, "Player not found");
        return;
    }
    if (vk_protected(target)) {
        vk_chat_to(client, "You cannot vote kick this player");
        return;
    }
    if (target->acct.account_id > 0 && target->acct.level >= VOTE_KICK_MAX_LVL) {
        char text[NET_CHAT_TEXT_SIZE];
        snprintf(text, sizeof(text), "Players above level %d cannot be vote kicked", VOTE_KICK_MAX_LVL - 1);
        vk_chat_to(client, text);
        return;
    }
    const int eligible = vk_eligible(target->id);
    if (eligible < 2) {
        vk_chat_to(client, "Not enough players to vote");
        return;
    }
    vote = calloc(1, sizeof(*vote));
    if (!vote) return;
    vote->timer = VOTE_KICK_TIMER_S;
    vote->player = target->player;
    vote->target_uid = target->id;
    vote->eligible = eligible;
    vote->votes_req = eligible < VOTE_KICK_REQ ? eligible : VOTE_KICK_REQ;
    vote->votes_for = 1; // the initiator's
    vote->voters[vote->voter_count++] = client->id;
    snprintf(vote->target, sizeof(vote->target), "%s", target->name);
    snprintf(vote->initiator, sizeof(vote->initiator), "%s", client->name);
    game->vote_kick = vote;
    server_log("[votekick] %s opened a vote against %s (%d needed of %d)", client->name, target->name, vote->votes_req, eligible);
    vk_broadcast(vote, 1, 1, 0); // "vk" target, votes, needed, true, voter
    if (vote->votes_for >= vote->votes_req) { // a 2-account lobby... never: eligible >= 2 and req >= 2
        vk_ban(target);
        net_kick_client(target->id, "Vote Kicked", "004002");
        vk_end(game, 1);
    }
}

// ---------------------------------------------------------------- K.P.D

void votekick_kpd_reply(const ClientConnection *to, const uint8_t code, const uint8_t status, const char *message, const char *name,
                        const char *json) {
    if (!to) return;
    uint8_t body[NET_EXT_BODY_MAX];
    NetKpdResponsePacket r;
    memset(&r, 0, sizeof(r));
    r.code = code;
    r.status = status;
    snprintf(r.message, sizeof(r.message), "%s", message ? message : "");
    snprintf(r.name, sizeof(r.name), "%s", name ? name : "");
    memcpy(body, &r, sizeof(r));
    size_t n = sizeof(r);
    if (json) {
        const size_t room = sizeof(body) - n;
        size_t len = strlen(json);
        if (len > room) len = room; // a longer list is cut (the client parses what fits)
        memcpy(body + n, json, len);
        n += len;
    }
    vk_send_ext(to, NET_EXT_KPD_RESPONSE, body, (uint16_t) n);
}

static void kpd_offer_new_call(int recent);

static KpdRecent *kpd_recent(const ClientConnection *target) {
    for (int i = 0; i < g_vk.recent_count; i++) {
        KpdRecent *r = &g_vk.recent[i];
        if (target->acct.account_id > 0 ? r->account_id == target->acct.account_id : !r->account_id && !strcmp(r->name, target->name)) return r;
    }
    return NULL;
}

void votekick_handle_kpd(ClientConnection *client, const uint8_t *body, const uint16_t length) {
    if (!client || length < sizeof(NetKpdCallPacket)) return;
    NetKpdCallPacket req;
    memcpy(&req, body, sizeof(req));
    if (!vk_is_account(client)) {
        votekick_kpd_reply(client, 0, NET_KPD_REFUSED, "Sign in to call the K.P.D", "", NULL);
        return;
    }
    if (req.tab != 0) { // the K.P.D window's lists (1 Active Calls, 2 Ban Logs, 3 Stats): moderators (and DEV) only
        if (!(client->acct.flags & NET_PLAYER_INFO_MOD) && !net_client_is_dev(client)) {
            votekick_kpd_reply(client, req.tab, NET_KPD_REFUSED, "K.P.D officers only", "", "[]");
            return;
        }
        if (req.tab > 3) { // v10's Demos / Reviewers come from its web API, not this socket
            votekick_kpd_reply(client, req.tab, NET_KPD_UNAVAILABLE, "Not available on this server", "", "[]");
            return;
        }
        accounts_kpd_request(client, req.tab, NULL);
        return;
    }
    if (client->acct.level < KPD_REP_LVL) {
        char text[64];
        snprintf(text, sizeof(text), "Requires Lvl %d", KPD_REP_LVL);
        votekick_kpd_reply(client, 0, NET_KPD_REFUSED, text, "", NULL);
        return;
    }
    ClientConnection *target = vk_by_id(req.uid);
    if (!target || target == client || !target->player) {
        votekick_kpd_reply(client, 0, NET_KPD_REFUSED, "Player not found", "", NULL);
        return;
    }
    if (vk_protected(target)) {
        votekick_kpd_reply(client, 0, NET_KPD_REFUSED, "You cannot report this player", "", NULL);
        return;
    }
    if (g_vk.last_kpd_call_at > 0.0f && g_server->now - g_vk.last_kpd_call_at < KPD_CALL_TIME_S) {
        votekick_kpd_reply(client, 0, NET_KPD_BUSY, "K.P.D line busy, try again in a moment", target->name, NULL);
        return;
    }
    KpdRecent *recent = kpd_recent(target);
    if (recent && g_server->now - recent->at < KPD_AUTO_TIME_S) {
        votekick_kpd_reply(client, 0, NET_KPD_BUSY, "Already reported to the K.P.D", target->name, NULL);
        return;
    }
    if (!recent) {
        recent = g_vk.recent_count < KPD_RECENT_MAX ? &g_vk.recent[g_vk.recent_count++] : &g_vk.recent[0];
        memset(recent, 0, sizeof(*recent));
        recent->account_id = target->acct.account_id > 0 ? target->acct.account_id : 0;
        snprintf(recent->name, sizeof(recent->name), "%s", target->name);
    }
    recent->at = g_server->now;
    recent->suspect_id = target->id;
    recent->caller_id = client->id;
    recent->call_id = 0;
    recent->rated = 0;
    g_vk.last_kpd_call_at = g_server->now;
    server_log("[kpd] %s called the K.P.D on %s", client->name, target->name);
    const int recent_index = (int) (recent - g_vk.recent);
    accounts_kpd_request(client, 0, target); // the record (kn_brain), then the answer
    kpd_offer_new_call(recent_index);       // an officer already here: the case now (its call id follows the record)
}

// NET_EXT_KPD_APPLY (v10 "kpdA" from kpdSubmit): v10's client rules again here (an account of level
// KPD_APPLY_LVL, not flagged, not an officer, a Discord name and a reason); kn_brain checks the
// reports (20 positive, rating above 0.7) and keeps it. The answer is policeRes code 6.
void votekick_handle_kpd_apply(ClientConnection *client, const uint8_t *body, const uint16_t length) {
    if (!client || length < sizeof(NetKpdApplyPacket)) return;
    NetKpdApplyPacket req;
    memcpy(&req, body, sizeof(req));
    req.discord[sizeof(req.discord) - 1] = 0;
    req.reason[sizeof(req.reason) - 1] = 0;
    const char *refused = !vk_is_account(client)                                ? "Sign in to apply for K.P.D"
                          : client->acct.level < KPD_APPLY_LVL || client->acct.hack ? "Requires Lvl 50"
                          : !req.discord[0]                                      ? "Invalid Discord"
                          : !req.reason[0]                                       ? "Invalid Reason"
                          : client->acct.flags & (NET_PLAYER_INFO_MOD | NET_PLAYER_INFO_DEV) ? "Already K.P.D" // the account's (v10 isMod), not a local / op DEV
                                                                                 : NULL;
    if (refused) {
        votekick_kpd_reply(client, 6, NET_KPD_REFUSED, refused, "", NULL);
        return;
    }
    server_log("[kpd] %s applies to the K.P.D", client->name);
    accounts_kpd_apply(client, req.discord, req.reason);
}

// NET_EXT_ADMIN_ACTION (v10 ADMIN_ACTIONS from the player list's Kick / Ban): a DEV's request. The
// sender's DEV right is checked here from the connection (never from the client's button), then the
// target is looked up by uid in this game and the kick / ban runs through the chat command layer,
// which checks the same right again. Anything else is refused with nothing changed.
void votekick_handle_admin(ClientConnection *client, const uint8_t *body, const uint16_t length) {
    if (!client || length < sizeof(NetAdminActionPacket)) return;
    NetAdminActionPacket req;
    memcpy(&req, body, sizeof(req));
    if (req.action != 0 && req.action != 1) return; // kill / tp / bring stay chat commands
    if (!net_client_is_dev(client)) {
        vk_chat_to(client, "Only DEV accounts can kick or ban from the player list");
        return;
    }
    if (!chat_command_admin_action(net_chat_host(), client->id, req.uid, req.action == 1)) {
        // the target left, or it is the sender: nothing was removed
        return;
    }
}

// ---------------------------------------------------------------- K.P.D officers

void votekick_kpd_recorded(const int suspect_id, const int64_t call_id) {
    for (int i = 0; i < g_vk.recent_count; i++) {
        if (g_vk.recent[i].suspect_id == suspect_id && !g_vk.recent[i].rated) g_vk.recent[i].call_id = call_id;
    }
}

static int kpd_is_officer(const ClientConnection *client) {
    return vk_is_account(client) && ((client->acct.flags & NET_PLAYER_INFO_MOD) || net_client_is_dev(client));
}

static void kpd_send_data(const ClientConnection *to, const int sus, const int caller) {
    NetKpdDataPacket d = {.sus = sus, .caller = caller};
    vk_send_ext(to, NET_EXT_KPD_DATA, &d, sizeof(d));
}

// A case an officer can still work: not rated, not older than KPD_CASE_TIME_S, the suspect still here
static int kpd_case_open(const KpdRecent *r) {
    return !r->rated && g_server->now - r->at <= KPD_CASE_TIME_S && vk_by_id(r->suspect_id) != NULL;
}

static int kpd_case_held(const int recent) {
    for (int k = 0; k < g_vk.officer_count; k++) {
        if (g_vk.officers[k].recent == recent && !g_vk.officers[k].rated && kpd_case_open(&g_vk.recent[recent])) return 1;
    }
    return 0;
}

// The officer's slot: NULL when it works an open case already (it keeps that one) or there is no
// room, else its old slot (a closed case: free for the next) or a new one.
static KpdOfficer *kpd_free_officer(const ClientConnection *client) {
    for (int i = 0; i < g_vk.officer_count; i++) {
        KpdOfficer *o = &g_vk.officers[i];
        if (o->conn_id != client->id) continue;
        const int working = !o->rated && o->recent >= 0 && o->recent < g_vk.recent_count && kpd_case_open(&g_vk.recent[o->recent]);
        return working ? NULL : o;
    }
    if (g_vk.officer_count >= KPD_OFFICERS_MAX) return NULL;
    KpdOfficer *o = &g_vk.officers[g_vk.officer_count++];
    memset(o, 0, sizeof(*o));
    o->conn_id = client->id;
    o->recent = -1;
    return o;
}

// The officer may work this call: not its own call, not a call on itself, nobody else on it
static int kpd_case_for(const ClientConnection *client, const int recent) {
    const KpdRecent *r = &g_vk.recent[recent];
    return kpd_case_open(r) && r->suspect_id != client->id && r->caller_id != client->id && !kpd_case_held(recent);
}

static void kpd_hand_over(const ClientConnection *client, KpdOfficer *o, const int recent) {
    o->recent = recent;
    o->since = g_server->now;
    o->rated = 0;
    const KpdRecent *r = &g_vk.recent[recent];
    const int caller = vk_by_id(r->caller_id) ? r->caller_id : -1;
    server_log("[kpd] officer %s takes the case of %s (call %lld)", client->name, r->name, (long long) r->call_id);
    kpd_send_data(client, r->suspect_id, caller);
}

// The newest open call this officer may work goes to it (v10 startGame kpdData). Called when an
// officer's account settles here (a join, a live sign-in) and, for the officers already in the lobby,
// when a call comes in (votekick_handle_kpd).
static int kpd_offer(const ClientConnection *client, const int only) {
    if (!kpd_is_officer(client)) return 0;
    int best = -1;
    for (int i = 0; i < g_vk.recent_count; i++) {
        if ((only < 0 || i == only) && kpd_case_for(client, i) && (best < 0 || g_vk.recent[i].at > g_vk.recent[best].at)) best = i;
    }
    if (best < 0) return 0;
    KpdOfficer *o = kpd_free_officer(client);
    if (!o) return 0;
    kpd_hand_over(client, o, best);
    return 1;
}

void votekick_officer_joined(const ClientConnection *client) {
    // a live sign-in again while the case is open: the case stands (kpd_free_officer)
    kpd_offer(client, -1);
}

// A call came in (KrunkNative: v10's officers answer it from the Distress Calls list and join; an
// officer already in this lobby gets it here, at once): the first officer with no open case takes it.
static void kpd_offer_new_call(const int recent) {
    for (size_t i = 0; i < g_server->client_count; i++) {
        if (kpd_offer(&g_server->clients[i], recent)) return;
    }
}

void votekick_handle_kpd_rate(ClientConnection *client, const uint8_t *body, const uint16_t length) {
    if (!client || length < sizeof(NetKpdRatePacket)) return;
    NetKpdRatePacket req;
    memcpy(&req, body, sizeof(req));
    KpdOfficer *o = NULL;
    for (int i = 0; i < g_vk.officer_count; i++) {
        if (g_vk.officers[i].conn_id == client->id) o = &g_vk.officers[i];
    }
    if (!o || !kpd_is_officer(client) || o->recent < 0 || o->recent >= g_vk.recent_count) return; // no case: v10 ignores it
    KpdRecent *r = &g_vk.recent[o->recent];
    if (o->rated || r->rated) {
        vk_chat_to(client, "Case already closed");
        return;
    }
    if (g_server->now - o->since < KPD_WATCH_TIME_S) { // kpdWatchTime: watch the suspect first
        vk_chat_to(client, "Watch the suspect a little longer");
        vk_send_ext(client, NET_EXT_KPD_RESET, NULL, 0); // "kpdf": the verdict is open again
        return;
    }
    o->rated = r->rated = 1;
    server_log("[kpd] officer %s rated %s: %s (call %lld)", client->name, r->name, req.tagged ? "tagged" : "not tagged", (long long) r->call_id);
    accounts_kpd_rate(client, r->call_id, req.tagged ? 1 : 0, r->name);
}
