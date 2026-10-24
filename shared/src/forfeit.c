// The team forfeit vote of a ranked match. Krunker v10.0.3 (deob-10.0.3.js): voteToForfeit and
// forfeitInitiators (startGame :219962-219963), the sync to the team (:215238-215257), the eligible
// voters (:215224-215238: a team player, or one disconnected for at most voteForfeitDisconnectGrace),
// updateForfeitVote (:221477-221514: votes pruned to the eligible voters, req = the eligible count,
// passed when num >= req, stalled with no eligible voter), resolveForfeit (:221518-221546: the team
// gets "vf" null and commands.forfeit.ended, the rest commands.forfeit.ended.other, then
// prepareForfeit of the other team, :221463-221473), the timer in the update loop (:222156-222163),
// voteForfeitTimer 60000 and voteForfeitDisconnectGrace 30000 (:19428, :22996-22997). Unchanged from
// v10.0.2.
//
// The start command is not in the v10 bundle (v10's server owns the chat commands). The v10 page
// tells players "Enter /ff to vote" (ui/index.html voteForfeitKeys), so /ff is the command and
// /forfeit an alias (chat_commands.c; the v7 i18n strings name it too). KrunkNative choices, not v10:
// the vote is ranked-only, a spectator (no team) cannot start one ("Must play atleast 1 round"
// is v10's text for that refusal here), a vote for one team blocks the other team, and voters are
// connections (uids), not accounts. The texts are v10's en.json strings (commands.forfeit.*).
#include <forfeit.h>
#include <shared.h>
#include <modes.h>
#include <net_ext.h>
#include "modes/mode_internal.h" // mode_team_score_add: the "ts" of prepareForfeit
#include <stdio.h>
#include <string.h>

#define FORFEIT_TEXT_NEXT "Must play atleast 1 round"                 // commands.forfeit.next
#define FORFEIT_TEXT_WAIT "Wait till match starts to forfeit"         // commands.forfeit.wait
#define FORFEIT_TEXT_VOTED "You already voted."                       // commands.forfeit.voted
#define FORFEIT_TEXT_LIMIT "You can only start one vote per game"     // commands.forfeit.limit
#define FORFEIT_TEXT_ENDED "Forfeiting Game"                          // commands.forfeit.ended
#define FORFEIT_TEXT_ENDED_OTHER "Other Team Forfeited"               // commands.forfeit.ended.other
#define FORFEIT_TEXT_RANKED "Forfeit votes are for ranked matches"    // KN: not in v10's strings
#define FORFEIT_TEXT_OTHER_TEAM "A forfeit vote is already running for the other team" // KN

// ---------------------------------------------------------------- helpers

static void forfeit_say(Game *game, const int uid, const char *text) {
    NetChatPacket packet;
    net_packet_chat(&packet, -1, NET_CHAT_SERVER, "Server", text);
    game_send(game, uid, NET_PACKET_CHAT, &packet, sizeof(packet));
}

static const Player *forfeit_present(const Game *game, const int uid) {
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *p = game->players[i];
        if (p && p->uid == uid) return p;
    }
    return NULL;
}

// The eligible voters of `team` (v10 getForfeitVoters): the team's players in the game, and the
// team's players who left within FORFEIT_GRACE_S and have not come back.
static int forfeit_leaver_fresh(const Game *game, const ForfeitLeaver *l) {
    return game->now - l->at <= FORFEIT_GRACE_S;
}

static int forfeit_eligible(const Game *game, const int uid, const int team) {
    const Player *p = forfeit_present(game, uid);
    if (p) return p->team == team;
    for (int i = 0; i < game->forfeit.leaver_count; i++) {
        const ForfeitLeaver *l = &game->forfeit.leavers[i];
        if (l->uid == uid && l->team == team) return forfeit_leaver_fresh(game, l);
    }
    return 0;
}

static int forfeit_eligible_count(const Game *game, const int team) {
    int n = 0;
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *p = game->players[i];
        if (p && p->team == team) n++;
    }
    for (int i = 0; i < game->forfeit.leaver_count; i++) {
        const ForfeitLeaver *l = &game->forfeit.leavers[i];
        if (l->team == team && forfeit_leaver_fresh(game, l) && !forfeit_present(game, l->uid)) n++;
    }
    return n;
}

static int forfeit_voted(const GameForfeit *f, const int uid) {
    for (int i = 0; i < f->voter_count; i++) {
        if (f->voters[i] == uid) return 1;
    }
    return 0;
}

static int forfeit_started(const GameForfeit *f, const int uid) {
    for (int i = 0; i < f->start_count; i++) {
        if (f->starts[i] == uid) return 1;
    }
    return 0;
}

// "vf" [initiator, num, req, isStart, ...] to every player of the vote's team
static void forfeit_send_team(Game *game, const NetVoteForfeitPacket *packet) {
    uint8_t buf[NET_EXT_HEADER_SIZE + sizeof(*packet)];
    const uint16_t n = net_ext_pack(buf, sizeof(buf), NET_EXT_VOTE_FORFEIT, packet, sizeof(*packet));
    if (!n) return;
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *p = game->players[i];
        if (p && p->team == packet->team) game_send(game, p->uid, NET_PACKET_EXT, buf, n);
    }
}

static void forfeit_packet(NetVoteForfeitPacket *out, const GameForfeit *f, const int opened) {
    memset(out, 0, sizeof(*out));
    out->active = f->active;
    out->opened = (uint8_t) opened;
    out->team = f->team;
    out->votes = f->voter_count;
    out->needed = f->req;
    snprintf(out->initiator, sizeof(out->initiator), "%s", f->initiator);
}

// v10 _63995: the team's counts ("vf" with isStart 0)
static void forfeit_sync(Game *game, const int opened) {
    NetVoteForfeitPacket packet;
    forfeit_packet(&packet, &game->forfeit, opened);
    forfeit_send_team(game, &packet);
}

// v10 updateForfeitVote: prune the votes to the eligible voters and set req to the eligible count.
// Returns 1 when the vote passed (every eligible voter agreed); *changed when a count moved. No
// eligible voter at all: stalled (no pass, no change; the timer runs out).
static int forfeit_update(Game *game, int *changed) {
    GameForfeit *f = &game->forfeit;
    *changed = 0;
    if (!f->active) return 0;
    const int req = forfeit_eligible_count(game, f->team);
    if (req <= 0) return 0;
    int kept = 0;
    for (int i = 0; i < f->voter_count; i++) {
        if (forfeit_eligible(game, f->voters[i], f->team)) f->voters[kept++] = f->voters[i];
    }
    *changed = kept != f->voter_count || req != f->req;
    f->voter_count = kept;
    f->votes = kept;
    f->req = req;
    return kept >= req;
}

// v10 resolveForfeit: the team gets "vf" null and "Forfeiting Game", everyone else "Other Team
// Forfeited"; the other team wins the round (prepareForfeit: forced in mode_pick_winner, then endGame)
static void forfeit_resolve(Game *game) {
    GameForfeit *f = &game->forfeit;
    const int team = f->team;
    f->active = 0;
    f->timer = 0.0f;
    NetVoteForfeitPacket none;
    forfeit_packet(&none, f, 0);
    none.active = 0;
    forfeit_send_team(game, &none);
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *p = game->players[i];
        if (!p) continue;
        forfeit_say(game, p->uid, p->team == team ? FORFEIT_TEXT_ENDED : FORFEIT_TEXT_ENDED_OTHER);
    }
    f->forced_team = team == 1 ? 2 : 1;
    // prepareForfeit (deob-10.0.3.js:221463-221473): the winning team's score and round wins become
    // ceil((bestOf || 1) / 2), the other team's none, and no round is left. bestOf is !!bstOfR (a boolean),
    // so that is 1 either way: the end screen and the ranked result read 1 - 0.
    GameMode *mode = game->mode;
    if (mode) {
        memset(mode->team_round_wins, 0, sizeof(mode->team_round_wins));
        mode->team_round_wins[f->forced_team] = 1;
        mode->has_round_wins = 1;
        mode->round_count = 0;
        for (int t = 1; t < MODE_TEAMS; t++) {
            const int want = t == f->forced_team ? 1 : 0;
            if (mode->team_scores[t] != want) mode_team_score_add(game, t, want - mode->team_scores[t]);
        }
    }
    game_end_round(game);
}

// ---------------------------------------------------------------- public

void forfeit_round_start(Game *game) {
    memset(&game->forfeit, 0, sizeof(game->forfeit));
}

void forfeit_player_left(Game *game, const Player *player) {
    GameForfeit *f = &game->forfeit;
    if (!player || player->team <= 0) return;
    for (int i = 0; i < f->leaver_count; i++) {
        if (f->leavers[i].uid == player->uid) {
            f->leavers[i] = (ForfeitLeaver) {.uid = player->uid, .team = player->team, .at = game->now};
            return;
        }
    }
    if (f->leaver_count >= FORFEIT_LEAVERS_MAX) { // full: the oldest leave goes
        memmove(&f->leavers[0], &f->leavers[1], (size_t) (FORFEIT_LEAVERS_MAX - 1) * sizeof(ForfeitLeaver));
        f->leaver_count--;
    }
    f->leavers[f->leaver_count++] = (ForfeitLeaver) {.uid = player->uid, .team = player->team, .at = game->now};
}

void forfeit_tick(Game *game, const float delta) {
    GameForfeit *f = &game->forfeit;
    if (!f->active) return;
    f->timer -= delta;
    if (f->timer <= 0.0f) { // v10: voteToForfeit = null, "vf" null to everyone
        f->active = 0;
        NetVoteForfeitPacket none;
        forfeit_packet(&none, f, 0);
        none.active = 0;
        uint8_t buf[NET_EXT_HEADER_SIZE + sizeof(none)];
        const uint16_t n = net_ext_pack(buf, sizeof(buf), NET_EXT_VOTE_FORFEIT, &none, sizeof(none));
        if (n) game_send(game, NET_TARGET_ALL, NET_PACKET_EXT, buf, n);
        return;
    }
    int changed = 0;
    const int passed = forfeit_update(game, &changed);
    if (changed) forfeit_sync(game, 0);
    if (passed) forfeit_resolve(game);
}

void forfeit_send_to(Game *game, const int uid) {
    const GameForfeit *f = &game->forfeit;
    const Player *p = forfeit_present(game, uid);
    if (!f->active || !p || p->team != f->team) return;
    NetVoteForfeitPacket packet;
    forfeit_packet(&packet, f, 0);
    uint8_t buf[NET_EXT_HEADER_SIZE + sizeof(packet)];
    const uint16_t n = net_ext_pack(buf, sizeof(buf), NET_EXT_VOTE_FORFEIT, &packet, sizeof(packet));
    if (n) game_send(game, uid, NET_PACKET_EXT, buf, n);
}

int forfeit_forced_team(const Game *game) {
    return game ? game->forfeit.forced_team : 0;
}

void forfeit_command(Game *game, const Player *sender) {
    if (!game || !sender) return;
    GameForfeit *f = &game->forfeit;
    const int uid = sender->uid;
    if (!game->is_ranked) {
        forfeit_say(game, uid, FORFEIT_TEXT_RANKED);
        return;
    }
    if (sender->team <= 0) {
        forfeit_say(game, uid, FORFEIT_TEXT_NEXT);
        return;
    }
    if (game->game_state != GAME_STATE_IN_PROGRESS) {
        forfeit_say(game, uid, FORFEIT_TEXT_WAIT);
        return;
    }

    if (f->active) { // a vote on the running one (the team's only)
        if (f->team != sender->team) {
            forfeit_say(game, uid, FORFEIT_TEXT_OTHER_TEAM);
            return;
        }
        if (forfeit_voted(f, uid)) {
            forfeit_say(game, uid, FORFEIT_TEXT_VOTED);
            return;
        }
        if (f->voter_count >= FORFEIT_VOTERS_MAX) return;
        f->voters[f->voter_count++] = uid;
        int changed = 0;
        const int passed = forfeit_update(game, &changed);
        char text[64];
        snprintf(text, sizeof(text), "Vote Submitted - %d/%d", f->votes, f->req); // v10 updateGameMessage(-1, null, ..., 2)
        forfeit_say(game, uid, text);
        forfeit_sync(game, 0);
        if (passed) forfeit_resolve(game);
        return;
    }

    if (forfeit_started(f, uid)) { // one start per player per game (v10 forfeitInitiators)
        forfeit_say(game, uid, FORFEIT_TEXT_LIMIT);
        return;
    }
    if (f->start_count >= FORFEIT_STARTS_MAX) return;
    f->starts[f->start_count++] = uid;

    memset(f->voters, 0, sizeof(f->voters));
    f->active = 1;
    f->team = (unsigned char) sender->team;
    f->timer = FORFEIT_TIMER_S;
    snprintf(f->initiator, sizeof(f->initiator), "%s", sender->name);
    f->voters[0] = uid; // the initiator's vote counts
    f->voter_count = 1;
    f->votes = 1;
    int changed = 0;
    const int passed = forfeit_update(game, &changed);
    NetVoteForfeitPacket opened;
    forfeit_packet(&opened, f, 1); // v10 isStart: the panel names the initiator
    forfeit_send_team(game, &opened);
    if (passed) forfeit_resolve(game);
}
