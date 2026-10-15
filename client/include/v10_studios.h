// Krunker v10.0.2's Studios on the v10 page (client/src/ui/v10_studios.c): the Studios window
// (openStudios, v10.js 215744 / 138200-138440) and the studio view (137011-137610) on the shared
// view stack, over the brain's /studios gapi routes (services/brain/studios.c).
#ifndef KRUNKNATIVE_V10_STUDIOS_H
#define KRUNKNATIVE_V10_STUDIOS_H

#include <html_dom.h>

int  v10_studios_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc); // -1 = not ours
int  v10_studios_key(int key);           // Escape on a Studios view: 1 = handled (closes the top one)
// The Games window's My Games tab (servers_ui.c): the pending invite count v10 shows next to
// "Studios" (window.setStudioInviteCount, #studioInviteCount), and the refresh it asks for when it
// renders the tab (at most every 30 s, signed in only)
int  v10_studios_invite_count(void);
void v10_studios_poll_invites(void);

#endif
