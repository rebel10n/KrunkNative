// The player list's vote kick and K.P.D call on the server (server/src/votekick.c),
// over the offline sandbox server (server_sandbox.h): raw protocol 1.4
// clients, the real NET_PACKET_EXT dispatch, the real game timer and kicks. Accounts are given
// to the connections after they join (account id, level), as kn_brain would have.
//   1. a vote that passes: opened (everyone gets "vk" opened with the initiator), the initiator's
//      vote counts, yes votes reach votes_req, the target gets ERROR "Vote Kicked" and is
//      vote-kick banned (votekick_banned) for voteKickBan, everyone gets "vk" ended (passed)
//   2. the cooldown: a new vote right after is refused
//   3. a vote that times out: voteKickTimer (35 s) of game time, "vk" ended (failed), no kick
//   4. the level cap: a target at voteKickMaxLvl (20) is refused, no vote opens
//   5. guests cannot vote, nobody can vote out DEV / the host, a target cannot vote on itself,
//      nobody votes twice
//   6. K.P.D: level 5 needed, the call answered "Reported <name>", kpdCallTime / kpdAutoTime
//   7. a K.P.D officer (a moderator account) joining gets the open case (kpdData sus / caller), a
//      verdict before kpdWatchTime (10 s) is refused ("kpdf"), then it counts once; nobody else
//      gets a rated case
//   8. the player list's Kick / Ban (NET_EXT_ADMIN_ACTION): a non-DEV's request is refused and
//      removes nobody; a DEV's kick and ban remove the target, a DEV cannot kick herself, a missing
//      uid removes nobody; /kick in chat still works
// Run from the repository root: ./<build>/votekick_test
#include <server_sandbox.h>
#include <server.h>
#include <server_votekick.h>
#include <server_accounts.h>
#include <shared.h>
#include <net_ext.h>
#include <modes.h>
#include <map_catalog.h>
#include <knet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
static int g_checks;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
    else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define MAX_RECEIVED 4096
#define CLIENTS 4 // the sandbox's SANDBOX_LINKS

typedef struct {
    int fd;
    int closed;
    NetPacketBuffer buffer;
    NetPacket packets[MAX_RECEIVED];
    int count;
} TestClient;

static TestClient g_clients[CLIENTS];

static void send_to(TestClient *c, const int type, const void *payload, const uint16_t length) {
    if (c->fd < NET_LOOPBACK_FD_BASE) return;
    net_send_packet(c->fd, (NetPacketType) type, payload, length);
}

static void drain(TestClient *c) {
    uint8_t bytes[4096];
    int n;
    while ((n = sandbox_receive(c->fd, bytes, sizeof(bytes))) > 0) {
        if (!net_buffer_push(&c->buffer, bytes, (size_t) n)) break;
        NetPacket packet;
        while (net_buffer_next(&c->buffer, &packet) > 0) {
            if (packet.type == NET_PACKET_PING && packet.length == sizeof(NetPingPacket)) send_to(c, NET_PACKET_PONG, packet.payload, packet.length);
            if (c->count < MAX_RECEIVED) c->packets[c->count++] = packet;
        }
    }
    if (n < 0) c->closed = 1;
}

static void pump(const int frames) {
    for (int f = 0; f < frames; f++) {
        sandbox_step(1.0 / 60.0);
        for (int i = 0; i < CLIENTS; i++) {
            if (g_clients[i].fd >= NET_LOOPBACK_FD_BASE && !g_clients[i].closed) drain(&g_clients[i]);
        }
    }
}

static void reset_received(void) {
    for (int i = 0; i < CLIENTS; i++) g_clients[i].count = 0;
}

static ClientConnection *conn(const TestClient *c) {
    for (size_t i = 0; i < g_server->client_count; i++) {
        if (c->fd >= NET_LOOPBACK_FD_BASE && g_server->clients[i].fd == c->fd - 1) return &g_server->clients[i]; // SANDBOX_SERVER_FD = client fd - 1
    }
    return NULL;
}

// the n-th EXT packet of `subtype` this client got (body copied into out), 1 when found
static int ext_nth(const TestClient *c, const uint16_t subtype, const int nth, void *out, const size_t size) {
    int seen = 0;
    for (int i = 0; i < c->count; i++) {
        if (c->packets[i].type != NET_PACKET_EXT) continue;
        uint16_t sub, bl;
        const uint8_t *body;
        if (!net_ext_unpack(c->packets[i].payload, c->packets[i].length, &sub, &body, &bl) || sub != subtype) continue;
        if (seen++ != nth) continue;
        memset(out, 0, size);
        memcpy(out, body, bl < size ? bl : size);
        return 1;
    }
    return 0;
}

static int ext_count(const TestClient *c, const uint16_t subtype) {
    char tmp[NET_EXT_BODY_MAX];
    int n = 0;
    while (ext_nth(c, subtype, n, tmp, sizeof(tmp))) n++;
    return n;
}

static int last_vk(const TestClient *c, NetVoteKickStatePacket *out) {
    const int n = ext_count(c, NET_EXT_VOTE_KICK_STATE);
    return n > 0 && ext_nth(c, NET_EXT_VOTE_KICK_STATE, n - 1, out, sizeof(*out));
}

static int got_error(const TestClient *c, const char *message) {
    for (int i = 0; i < c->count; i++) {
        if (c->packets[i].type != NET_PACKET_ERROR || c->packets[i].length < sizeof(NetErrorPacket)) continue;
        NetErrorPacket e;
        memcpy(&e, c->packets[i].payload, sizeof(e));
        e.message[sizeof(e.message) - 1] = 0;
        if (strstr(e.message, message)) return 1;
    }
    return 0;
}

static int got_chat(const TestClient *c, const char *text) {
    for (int i = 0; i < c->count; i++) {
        if (c->packets[i].type != NET_PACKET_CHAT || c->packets[i].length < sizeof(NetChatPacket)) continue;
        NetChatPacket p;
        memcpy(&p, c->packets[i].payload, sizeof(p));
        p.text[sizeof(p.text) - 1] = 0;
        if (strstr(p.text, text)) return 1;
    }
    return 0;
}

static void vote(TestClient *c, const char *target, const int yes) {
    NetVoteKickPacket v;
    memset(&v, 0, sizeof(v));
    snprintf(v.target, sizeof(v.target), "%s", target);
    v.vote = (uint8_t) yes;
    uint8_t buf[NET_EXT_HEADER_SIZE + sizeof(v)];
    const uint16_t n = net_ext_pack(buf, sizeof(buf), NET_EXT_VOTE_KICK, &v, sizeof(v));
    send_to(c, NET_PACKET_EXT, buf, n);
}

static void kpd(TestClient *c, const int uid) {
    const NetKpdCallPacket k = {.tab = 0, .uid = uid};
    uint8_t buf[NET_EXT_HEADER_SIZE + sizeof(k)];
    const uint16_t n = net_ext_pack(buf, sizeof(buf), NET_EXT_KPD_CALL, &k, sizeof(k));
    send_to(c, NET_PACKET_EXT, buf, n);
}

// the player list's Kick (0) / Ban (1) of Game uid `uid` (NET_EXT_ADMIN_ACTION, v10 ADMIN_ACTIONS)
static void admin_action(TestClient *c, const int uid, const int action) {
    const NetAdminActionPacket a = {.uid = uid, .action = (uint8_t) action};
    uint8_t buf[NET_EXT_HEADER_SIZE + sizeof(a)];
    const uint16_t n = net_ext_pack(buf, sizeof(buf), NET_EXT_ADMIN_ACTION, &a, sizeof(a));
    send_to(c, NET_PACKET_EXT, buf, n);
}

// a chat line (NET_PACKET_CHAT_SEND), e.g. "/kick Bob"
static void chat(TestClient *c, const char *text) {
    NetChatSendPacket packet;
    memset(&packet, 0, sizeof(packet));
    snprintf(packet.text, sizeof(packet.text), "%s", text);
    send_to(c, NET_PACKET_CHAT_SEND, &packet, (uint16_t) (strlen(packet.text) + 1));
}

static void kpd_apply(TestClient *c, const char *discord, const char *reason) {
    NetKpdApplyPacket k;
    memset(&k, 0, sizeof(k));
    snprintf(k.discord, sizeof(k.discord), "%s", discord);
    snprintf(k.reason, sizeof(k.reason), "%s", reason);
    uint8_t buf[NET_EXT_HEADER_SIZE + sizeof(k)];
    const uint16_t n = net_ext_pack(buf, sizeof(buf), NET_EXT_KPD_APPLY, &k, sizeof(k));
    send_to(c, NET_PACKET_EXT, buf, n);
}

static void kpd_rate(TestClient *c, const int tagged) {
    const NetKpdRatePacket k = {.tagged = (uint8_t) tagged};
    uint8_t buf[NET_EXT_HEADER_SIZE + sizeof(k)];
    const uint16_t n = net_ext_pack(buf, sizeof(buf), NET_EXT_KPD_RATE, &k, sizeof(k));
    send_to(c, NET_PACKET_EXT, buf, n);
}

static void join(TestClient *c) {
    memset(c, 0, sizeof(*c));
    c->fd = sandbox_connect();
    const NetHelloPacket hello = {NET_PROTOCOL_MAJOR, NET_PROTOCOL_MINOR_EXT, 0, 0};
    send_to(c, NET_PACKET_HELLO, &hello, sizeof(hello));
    NetJoinPacket j;
    memset(&j, 0, sizeof(j));
    send_to(c, NET_PACKET_JOIN, &j, (uint16_t) NET_JOIN_PACKET_V2_SIZE);
    NetEntPacket ent = {.class_index = 0, .secondary_index = -1, .cosmetics = PLAYER_COSMETICS_NONE, .slid_cont = 1, .lag_comp = 1.0f};
    send_to(c, NET_PACKET_ENT, &ent, (uint16_t) NET_ENT_PACKET_V7_SIZE);
    pump(2);
}

// what kn_brain's auth would have given the connection
static void give_account(TestClient *c, const int account_id, const int level, const char *name) {
    ClientConnection *cc = conn(c);
    if (!cc) return;
    cc->acct.account_id = account_id;
    cc->acct.level = level;
    cc->acct.flags = 0;
    cc->local = 0; // the sandbox's connections are the offline player's (DEV): these are remote players
    cc->op = 0;
    snprintf(cc->acct.username, sizeof(cc->acct.username), "%s", name);
    snprintf(cc->name, sizeof(cc->name), "%s", name);
    if (cc->player) snprintf(cc->player->name, sizeof(cc->player->name), "%s", name);
}

static void cleanup_vote(void) { // the next case starts with no vote and no cooldown
    pump((int) (60 * (VOTE_KICK_TIMER_S + VOTE_KICK_COOLDOWN_S + 1.0f)));
    reset_received();
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const cJSON *burg = map_catalog_json((size_t) map_catalog_index_of_key("311_burg"));
    static const cJSON *maps[1];
    maps[0] = burg;
    const SandboxConfig cfg = {.maps = maps, .map_count = 1, .map_index = 0, .mode_id = MODE_ID_FFA};
    CHECK(burg && sandbox_start(&cfg), "sandbox FFA on Burg");
    for (int i = 0; i < CLIENTS; i++) join(&g_clients[i]);
    pump(60);
    TestClient *A = &g_clients[0], *B = &g_clients[1], *C = &g_clients[2], *D = &g_clients[3];
    for (int i = 0; i < CLIENTS; i++) CHECK(conn(&g_clients[i]) && conn(&g_clients[i])->player, "client %d spawned", i);
    give_account(A, 101, 30, "Alice");
    give_account(B, 102, 6, "Bob");
    give_account(C, 103, 9, "Carol");
    if (conn(D)) conn(D)->local = conn(D)->op = 0; // D: a guest first (not the sandbox's local DEV)
    reset_received();

    // ---- 5a. a guest cannot open a vote
    vote(D, "Carol", 1);
    pump(5);
    CHECK(!g_server->game.vote_kick && got_chat(D, "Sign in to vote kick"), "a guest cannot vote kick");
    give_account(D, 104, 3, "Dave"); // the target of the first vote (level 3 < 20)
    reset_received();

    // ---- 1. a vote that passes and bans
    vote(A, "Dave", 1);
    pump(5);
    NetVoteKickStatePacket st;
    CHECK(g_server->game.vote_kick != NULL, "Alice opens a vote against Dave");
    const int eligible = 3; // Alice, Bob, Carol: the accounts other than Dave
    CHECK(last_vk(B, &st) && st.active && st.opened && st.votes == 1 && st.needed == eligible && !strcmp(st.target, "Dave") &&
              !strcmp(st.voter, "Alice"),
          "everyone gets vk opened: Dave 1/%d by Alice (got %d/%d)", eligible, st.votes, st.needed);
    CHECK(last_vk(D, &st) && st.active, "the target sees it");
    // ---- 5b. no second vote, no self vote, no double vote
    vote(D, "Dave", 1);
    vote(A, "Dave", 1);
    pump(5);
    CHECK(g_server->game.vote_kick && g_server->game.vote_kick->votes_for == 1, "the target and a repeat vote do not count (%d)",
          g_server->game.vote_kick ? g_server->game.vote_kick->votes_for : -1);
    vote(B, "Dave", 1);
    pump(5);
    CHECK(last_vk(C, &st) && st.active && !st.opened && st.votes == 2, "Bob votes yes: 2/%d", st.needed);
    CHECK(got_chat(B, "Vote Submitted - 2/3"), "Bob: Vote Submitted - 2/3");
    vote(C, "Dave", 1);
    pump(10);
    CHECK(got_error(D, "Vote Kicked"), "Dave is kicked: ERROR Vote Kicked");
    CHECK(last_vk(A, &st) && !st.active && st.passed, "vk ended (passed) for everyone");
    CHECK(!g_server->game.vote_kick, "no vote left");
    pump(30);
    CHECK(!conn(D), "Dave's connection is gone");
    char why[96] = "";
    CHECK(votekick_banned(104, "", why, sizeof(why)) && strstr(why, "30 min"), "Dave's account is vote-kick banned: \"%s\"", why);
    CHECK(!votekick_banned(101, "", NULL, 0), "Alice is not");
    sandbox_disconnect(D->fd); // the client end lets go of the closed link too
    D->fd = -1;
    pump(5);

    // ---- 2. the cooldown
    reset_received();
    vote(B, "Carol", 1);
    pump(5);
    CHECK(!g_server->game.vote_kick && got_chat(B, "just ended"), "a new vote right after is refused (cooldown)");
    cleanup_vote();

    // ---- 3. a vote that times out (Carol is the target; Alice and Bob may vote)
    join(D); // a fourth account so a 1-vote stall cannot pass: eligible 3, needed 3
    pump(30);
    CHECK(conn(D) && conn(D)->player, "a new player joins in Dave's place");
    give_account(D, 105, 4, "Dan");
    reset_received();
    vote(B, "Carol", 1);
    pump(5);
    CHECK(g_server->game.vote_kick && g_server->game.vote_kick->votes_req == 3, "Bob opens a vote against Carol (needs 3)");
    pump((int) (60 * (VOTE_KICK_TIMER_S - 2.0f)));
    CHECK(g_server->game.vote_kick != NULL, "still running after 33 s");
    pump(60 * 3);
    CHECK(!g_server->game.vote_kick, "gone after voteKickTimer (35 s)");
    CHECK(last_vk(A, &st) && !st.active && !st.passed, "vk ended (failed) for everyone");
    CHECK(conn(C) && !got_error(C, "Vote Kicked"), "Carol is still in the game");
    CHECK(got_chat(A, "Vote kick failed"), "the lobby is told the vote failed");
    cleanup_vote();

    // ---- 3b. a vote that fails on no votes
    vote(A, "Carol", 1);
    pump(5);
    vote(B, "Carol", 0);
    vote(D, "Carol", 0);
    pump(10);
    CHECK(!g_server->game.vote_kick && last_vk(B, &st) && !st.active && !st.passed && conn(C), "two no votes: it cannot pass, it fails");
    cleanup_vote();

    // ---- 4. the level cap: voteKickMaxLvl 20 (Alice is 30)
    vote(B, "Alice", 1);
    pump(5);
    CHECK(!g_server->game.vote_kick && got_chat(B, "above level 19"), "Alice (level 30) cannot be vote kicked");
    give_account(C, 103, VOTE_KICK_MAX_LVL - 1, "Carol");
    reset_received();
    vote(B, "Carol", 1);
    pump(5);
    CHECK(g_server->game.vote_kick != NULL, "Carol at level 19 can");
    cleanup_vote();

    // ---- 5c. DEV / the host are protected
    conn(A)->acct.flags |= NET_PLAYER_INFO_DEV;
    give_account(A, 101, 2, "Alice");
    conn(A)->acct.flags |= NET_PLAYER_INFO_DEV;
    vote(B, "Alice", 1);
    pump(5);
    CHECK(!g_server->game.vote_kick && got_chat(B, "cannot vote kick this player"), "a DEV cannot be vote kicked");
    conn(A)->acct.flags = 0;
    give_account(A, 101, 30, "Alice");
    reset_received();

    // ---- 6. K.P.D
    kpd(D, conn(C)->player->uid); // Dan is level 4
    pump(5);
    NetKpdResponsePacket r;
    CHECK(ext_nth(D, NET_EXT_KPD_RESPONSE, 0, &r, sizeof(r)) && r.status == NET_KPD_REFUSED && strstr(r.message, "Lvl 5"),
          "level 4: Requires Lvl 5 (\"%s\")", r.message);
    conn(D)->acct.account_id = 0; // Dan as a guest for a moment
    kpd(D, conn(C)->player->uid);
    pump(5);
    CHECK(ext_nth(D, NET_EXT_KPD_RESPONSE, 1, &r, sizeof(r)) && r.status == NET_KPD_REFUSED && strstr(r.message, "Sign in"), "a guest cannot call");
    conn(D)->acct.account_id = 105;
    kpd(B, conn(C)->player->uid);
    pump(5);
    CHECK(ext_nth(B, NET_EXT_KPD_RESPONSE, 0, &r, sizeof(r)) && r.status == NET_KPD_REPORTED && !strcmp(r.name, "Carol") &&
              !strcmp(r.message, "Reported Carol to the K.P.D"),
          "Bob reports Carol: \"%s\"", r.message);
    kpd(A, conn(C)->player->uid);
    pump(5);
    CHECK(ext_nth(A, NET_EXT_KPD_RESPONSE, 0, &r, sizeof(r)) && r.status == NET_KPD_BUSY, "kpdCallTime: a call 5 frames later is busy");
    pump((int) (60 * (KPD_CALL_TIME_S + 1.0f)));
    reset_received();
    kpd(A, conn(C)->player->uid);
    pump(5);
    CHECK(ext_nth(A, NET_EXT_KPD_RESPONSE, 0, &r, sizeof(r)) && r.status == NET_KPD_BUSY && strstr(r.message, "Already"),
          "kpdAutoTime: Carol already reported (\"%s\")", r.message);
    kpd(A, conn(A)->player->uid);
    pump(5);
    CHECK(ext_nth(A, NET_EXT_KPD_RESPONSE, 1, &r, sizeof(r)) && r.status == NET_KPD_REFUSED, "nobody reports themselves");

    // ---- 7. a K.P.D officer takes the case (Bob reported Carol)
    const int carol = conn(C)->player->uid, bob = conn(B)->player->uid;
    sandbox_disconnect(D->fd); // Dan leaves, an officer joins in his place
    D->fd = -1;
    pump(10);
    join(D);
    pump(30);
    give_account(D, 106, 60, "Officer");
    conn(D)->acct.flags |= NET_PLAYER_INFO_MOD;
    reset_received();
    votekick_officer_joined(conn(D)); // what acc_set_ready / a live sign-in does once the account settles
    pump(2);
    NetKpdDataPacket kd;
    CHECK(ext_nth(D, NET_EXT_KPD_DATA, 0, &kd, sizeof(kd)) && kd.sus == carol && kd.caller == bob,
          "the officer gets kpdData: sus Carol (%d), caller Bob (%d)", kd.sus, kd.caller);
    CHECK(!ext_count(A, NET_EXT_KPD_DATA) && !ext_count(C, NET_EXT_KPD_DATA), "nobody else gets it");
    votekick_officer_joined(conn(D));
    pump(2);
    CHECK(ext_count(D, NET_EXT_KPD_DATA) == 1, "the same officer settling again keeps one case");
    kpd_rate(D, 1);
    pump(5);
    CHECK(ext_count(D, NET_EXT_KPD_RESET) == 1 && got_chat(D, "a little longer"), "a verdict before kpdWatchTime is refused (kpdf)");
    pump((int) (60 * (KPD_WATCH_TIME_S + 0.5f)));
    reset_received();
    kpd_rate(D, 1);
    pump(5);
    CHECK(!ext_count(D, NET_EXT_KPD_RESET) && !got_chat(D, "longer"), "after 10 s the verdict (tagged) counts");
    kpd_rate(D, 0);
    pump(5);
    CHECK(got_chat(D, "Case already closed"), "a second verdict: Case already closed");
    conn(A)->acct.flags |= NET_PLAYER_INFO_MOD; // another officer: the only case is closed
    votekick_officer_joined(conn(A));
    pump(2);
    CHECK(!ext_count(A, NET_EXT_KPD_DATA), "a rated case is nobody's any more");
    conn(A)->acct.flags = 0;
    conn(D)->acct.flags = 0;
    kpd_rate(C, 1);
    pump(5);
    CHECK(!got_chat(C, "Case") && !ext_count(C, NET_EXT_KPD_RESET), "a player without a case cannot rate");

    // ---- 7b. an officer already in the lobby when the call is placed gets the case at once
    pump((int) (60 * (KPD_CALL_TIME_S + 1.0f))); // the lobby's line is free again
    conn(D)->acct.flags |= NET_PLAYER_INFO_MOD;  // the officer stays in the lobby, its last case closed
    reset_received();
    kpd(A, bob); // Alice calls the K.P.D on Bob
    pump(5);
    CHECK(ext_nth(A, NET_EXT_KPD_RESPONSE, 0, &r, sizeof(r)) && r.status == NET_KPD_REPORTED, "Alice reports Bob");
    CHECK(ext_nth(D, NET_EXT_KPD_DATA, 0, &kd, sizeof(kd)) && kd.sus == bob && kd.caller == conn(A)->player->uid,
          "the officer in the lobby gets kpdData at once: sus Bob (%d), caller Alice", kd.sus);
    CHECK(!ext_count(A, NET_EXT_KPD_DATA) && !ext_count(B, NET_EXT_KPD_DATA) && !ext_count(C, NET_EXT_KPD_DATA), "nobody else gets it");
    votekick_officer_joined(conn(D)); // a live sign-in again: the case stands, no second handover
    pump(2);
    CHECK(ext_count(D, NET_EXT_KPD_DATA) == 1, "a second settle keeps the one case");
    pump((int) (60 * (KPD_CALL_TIME_S + 1.0f)));
    reset_received();
    kpd(C, conn(A)->player->uid); // a second call while the officer works Bob's case
    pump(5);
    CHECK(ext_nth(C, NET_EXT_KPD_RESPONSE, 0, &r, sizeof(r)) && r.status == NET_KPD_REPORTED, "Carol reports Alice");
    CHECK(!ext_count(D, NET_EXT_KPD_DATA), "an officer with an open case keeps it (no second case)");
    pump((int) (60 * (KPD_WATCH_TIME_S + 0.5f)));
    reset_received();
    kpd_rate(D, 0);
    pump(5);
    CHECK(!ext_count(D, NET_EXT_KPD_RESET) && !got_chat(D, "longer") && !got_chat(D, "closed"), "the officer's verdict on Bob counts");
    conn(A)->acct.flags |= NET_PLAYER_INFO_MOD; // an officer who is the suspect of the open call: never their own case
    votekick_officer_joined(conn(A));
    pump(2);
    CHECK(!ext_count(A, NET_EXT_KPD_DATA), "nobody gets the case on themselves");
    conn(C)->acct.flags |= NET_PLAYER_INFO_MOD; // nor the call they placed
    votekick_officer_joined(conn(C));
    pump(2);
    CHECK(!ext_count(C, NET_EXT_KPD_DATA), "nobody gets their own call");
    votekick_officer_joined(conn(D)); // the officer's case is closed: the open call on Alice is next
    pump(2);
    CHECK(ext_nth(D, NET_EXT_KPD_DATA, 0, &kd, sizeof(kd)) && kd.sus == conn(A)->player->uid, "a free officer settling takes the next open call");
    conn(A)->acct.flags = conn(C)->acct.flags = conn(D)->acct.flags = 0;

    // ---- 7c. the K.P.D application ("kpdA") and /kpd: the server's rules (no coordinator here)
    reset_received();
    kpd_apply(A, "alice#1", "I hunt hackers"); // Alice is level 30
    pump(3);
    CHECK(ext_nth(A, NET_EXT_KPD_RESPONSE, 0, &r, sizeof(r)) && r.code == 6 && r.status == NET_KPD_REFUSED && strstr(r.message, "Lvl 50"),
          "an application below level 50 is refused (\"%s\")", r.message);
    conn(A)->acct.level = 60;
    reset_received();
    kpd_apply(A, "", "I hunt hackers");
    pump(3);
    CHECK(ext_nth(A, NET_EXT_KPD_RESPONSE, 0, &r, sizeof(r)) && r.code == 6 && strstr(r.message, "Invalid Discord"), "no Discord: refused");
    conn(A)->acct.flags |= NET_PLAYER_INFO_MOD;
    reset_received();
    kpd_apply(A, "alice#1", "again");
    pump(3);
    CHECK(ext_nth(A, NET_EXT_KPD_RESPONSE, 0, &r, sizeof(r)) && r.code == 6 && strstr(r.message, "Already K.P.D"), "an officer: Already K.P.D");
    conn(A)->acct.flags = 0;
    reset_received();
    kpd_apply(A, "alice#1", "I hunt hackers");
    pump(3);
    CHECK(ext_nth(A, NET_EXT_KPD_RESPONSE, 0, &r, sizeof(r)) && r.code == 6 && r.status == NET_KPD_UNAVAILABLE,
          "a valid application with no coordinator: unreachable (\"%s\")", r.message);
    conn(A)->acct.level = 30;
    conn(B)->op = 1; // DEV (the sandbox's open perms would let anyone run it; the DEV rule is chat_commands.c's)
    chat(B, "/kpd Carol");
    pump(3);
    CHECK(got_chat(B, "Usage: /kpd"), "/kpd without on|off: usage");
    reset_received();
    chat(B, "/kpd Carol on");
    pump(3);
    CHECK(got_chat(B, "unreachable"), "/kpd Carol on with no coordinator: unreachable");
    conn(B)->op = 0;

    // ---- 8. the player list's Kick / Ban (NET_EXT_ADMIN_ACTION, v10 ADMIN_ACTIONS): DEV only
    reset_received();
    CHECK(conn(A) && conn(B) && conn(C), "Alice, Bob and Carol are here for the admin cases");
    admin_action(B, conn(A)->id, 0); // a non-DEV account asks to kick Alice
    admin_action(A, conn(B)->id, 0); // Alice is not DEV yet (the 5c / 7 flags are cleared): refused
    admin_action(A, conn(C)->id, 1); // ... and a ban of Carol
    pump(5);
    CHECK(conn(A) && conn(B) && conn(C) && !got_error(B, "Kicked") && !got_error(C, "Banned"),
          "a non-DEV's kick and ban change nothing: Bob and Carol stay");
    CHECK(got_chat(A, "Only DEV accounts can kick or ban from the player list"), "the refused sender is told why");
    CHECK(!got_error(A, "Kicked") && conn(A), "a non-DEV is not kicked by another non-DEV's request");
    reset_received();
    conn(A)->acct.flags |= NET_PLAYER_INFO_DEV; // Alice is DEV now (the server re-reads it on every request)
    admin_action(A, conn(A)->id, 0);
    admin_action(A, 99999, 0); // no such player
    pump(5);
    CHECK(conn(A) && got_chat(A, "Cannot kick yourself"), "a DEV cannot kick herself");
    CHECK(conn(C) != NULL, "a missing target removes nobody");
    const int kick_uid = conn(C)->id;
    admin_action(A, kick_uid, 0);
    pump(5);
    CHECK(got_error(C, "Kicked") && !conn(C), "a DEV's kick from the list kicks Carol");
    chat(A, "/kick Bob"); // the chat command is unchanged
    pump(5);
    CHECK(got_error(B, "Kicked") && !conn(B), "/kick Bob in chat still kicks Bob");
    if (conn(D)) {
        admin_action(A, conn(D)->id, 1);
        pump(5);
        CHECK(got_error(D, "Banned") && !conn(D), "a DEV's ban from the list bans Dave's connection");
    }
    conn(A)->acct.flags = 0;

    for (int i = 0; i < CLIENTS; i++) {
        if (g_clients[i].fd >= NET_LOOPBACK_FD_BASE) sandbox_disconnect(g_clients[i].fd);
    }
    sandbox_stop();
    printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures ? 1 : 0;
}
