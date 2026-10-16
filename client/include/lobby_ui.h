#pragma once
// WP-E client (doc/v7/map-logic.md §5.2-5.4): the lobby packets of block 180-189 and the end
// screen's match vote cards. client/src/net/lobby_events.c.
//   GAME_CONFIG    -> game_config_receive: the server's settings / class, weapon, effect tables
//   NETWORK_DATA   -> game_network_receive: the round's send rate ("k" sendRateA), viewDis
//   MATCH_VOTE_STATE -> #matchVoteHolder (index.js f1915 / updMatchVote), clicks: voteOnMatch
#include <client.h>
#include <html_dom.h>

// One of the lobby packets: 1 when handled (also when malformed), 0 when `type` is not one.
int lobby_events_handle(Client*, int type, const void *payload, uint16_t length);
void lobby_vote_reset(void);                         // INIT: the vote cards go away
const NetMatchVoteStatePacket *lobby_vote_state(void); // the latest state (count 0: none)
int lobby_vote_mine(void);                           // our vote (entry index), -1 none
// voteOnMatch("mode,map"): MATCH_VOTE to the server (v7-protocol servers and offline). 1 = sent.
int lobby_vote_send(Client*, int mode_id, int map_index);
// The #matchVoteHolder cards for the current state (malloc'd, the caller frees), NULL when
// there is nothing to vote on (fewer than 2 picks).
char *lobby_vote_cards_html(void);
// f1915: fills #matchVoteHolder (shown inline-block) or hides it; 1 when cards are shown.
int lobby_vote_show(void);
// Page handler "voteOnMatch" (args: the "mode,map" string of the card).
int lobby_vote_handler(void *user, const HtmlEvent *ev, const HtmlArg *args, int argc);

// Krunker v7.1.4 comp menu (#mMenuHolComp, client/src/ui/comp_ui.c): the lobby table of a comp
// game (ranked matches) from NET_PACKET_COMP_LOBBY, f1904 / f1892, updSyncDat.
int  comp_ui_packet(Client*, const void *payload, uint16_t length); // NET_PACKET_COMP_LOBBY
void comp_ui_round(Client*);                     // f1904 at every round start: the comp or the default menu
void comp_ui_match_gate(Client*);                // v10 INIT (g:219155, g:219431): the spectate button and the comp box's title by isRanked
void comp_ui_refresh(Client*);                   // f1892: lists, class locks, Join / Leave Team
void comp_ui_message(const char *text);          // io_updateGameMessage: #compTimBox
void comp_ui_class_selected(Client*, int index); // selectClass: updSyncDat("c", i)
int  comp_ui_toggle_team(void *user, const HtmlEvent *ev, const HtmlArg *args, int argc); // toggleTeam(t)
