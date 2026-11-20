// The menu's vote on the hosted community map (#mapVote) and the loaded mod (#modVote): client/src/ui/creation_vote.c.
#ifndef KRUNKNATIVE_CREATION_VOTE_H
#define KRUNKNATIVE_CREATION_VOTE_H

#include <html_dom.h>

// NET_EXT_KN_CREATION_INFO: the published map this game hosts (map_id 0 = none: the panel empties).
void creation_vote_set_map(int map_id, const char *name, const char *creator);
// The player's loaded mod (mod_id 0 = none or a local zip: #modVote hides).
void creation_vote_set_mod(int mod_id, const char *name, const char *creator);
// voteMap(id, v) / voteMod(id, v); -1 = not ours.
int creation_vote_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc);
// The choice after pressing `press` (1 up, -1 down) with `current` chosen: the same side again clears it.
int creation_vote_next(int current, int press);
int creation_vote_map_id(void);
int creation_vote_mod_id(void);

#endif
