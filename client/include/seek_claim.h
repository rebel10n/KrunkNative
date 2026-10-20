#ifndef KN_SEEK_CLAIM_H
#define KN_SEEK_CLAIM_H

// Which matchmaking answer may put the player in a lobby (servers_ui.c), without a window.
//
// Every request the player starts (a seek, a host, a play) takes an action number when it starts. A
// request that continues it (the seek a ping list or a region race sends after its answer, the next try
// of a full lobby) keeps the number of the action it belongs to. An answer that enters a lobby claims its
// action. An answer whose action is older than the claimed one was overtaken: the player entered a lobby
// that a later action asked for (a community map's host, a join by hand), so that answer is dropped. It
// neither moves the player to another lobby nor shows an error on the lobby the player is in.
//
// Answers that are not answers to a request (a server's redirect, a join ticket) claim a new action.
// Action 0 means "none": it is never overtaken.

typedef struct {
    unsigned started; // the last action number given out
    unsigned claimed; // the newest action whose answer entered a lobby (0 = none yet)
} SeekClaims;

// A new action (the next number, never 0).
unsigned seek_claim_start(SeekClaims *c);

// 1 when the action's answer is overtaken: a newer action has entered a lobby since it was started, or
// the action's own lobby was entered already (a second answer of the same request: a duplicate).
int seek_claim_overtaken(const SeekClaims *c, unsigned action);

// The answer of action entered a lobby. action 0 (no request asked for it) takes a new action. Returns
// the action that claimed the lobby.
unsigned seek_claim_enter(SeekClaims *c, unsigned action);

// 1 when a failed attempt of action may show "Could not reach the matchmaker": only the player's current
// action (the newest one given out), while the player is not in a lobby (in_lobby = 0), and not overtaken.
// A failure of an older action, or one that finds the player in a lobby, is only logged.
int seek_claim_error_shown(const SeekClaims *c, unsigned action, int in_lobby);

#endif
