#pragma once
// Krunker v10.0.2 Invite / Join (client/src/ui/v10_party.c). v10 has no party system: these are
// its ways into a friend's game, all ending in the matchmaker (/mm/seek-game?game=<id>, the
// krunk.io edges) with a fresh join ticket:
//   - the menu's matchInfoHolder "Invite - Join" (copyInviteLink, openJoinWindow)
//   - the Join window (windows[23] = showWindow(24): #gameURL + joinGame())
//   - a friend's "Join" (the social panel's conversation header: switchServer(gameInstanceId);
//     the profile window's Friends tab: openURL('/?game=<gid>', 1))
//   - the server password popup (gameCode() / submitGameCode()) when a game needs one
#include <html_dom.h>

// window.copyInviteLink(toast): https://<host>/?game=<gameId> to the clipboard, #inviteButton
// says "Copied URL" for 1250 ms; toast (the competitive menu's button) adds the chat line
// "Link Copied. Send it to someone to invite them".
void v10_party_copy_invite_link(int toast);
// window.openJoinWindow(a, skipClipboard): a game link / code on the clipboard joins it, anything
// else (or no clipboard, skipClipboard) opens the Join window.
void v10_party_open_join_window(int skip_clipboard);
// v10 _72569(text) (joinGame(): #gameURL's value): a game link "[http(s)://][www.]<site>/?game=<id>"
// or a bare "NODE:lobby" code joins that game; anything else (re)opens the Join window.
void v10_party_join_text(const char *text);
// joinGame() (the Join window's field): the same parse, but a refused text only shows its notice and the
// window stays open with the typed text.
void v10_party_join_field(const char *text);
// A friend's "Join" (switchServer(gameInstanceId)): the game id from the friends list / the
// "presence:game" push (kn_brain game_presence.c). 1 = joining, 0 = no matchmaker here.
// A game that needs a password asks for it once the server refuses the join (the popup below).
int v10_party_join_game(const char *game_id);
// openURL(url, newTab) of a same-site game link (the profile window's Friends tab "Join",
// '/?game=<gid>'): joins it in place instead of opening a browser. 1 = it was one (handled).
int v10_party_open_url(const char *url);
// window.gameCode(): v10's "Enter Server Password to join" popup (#genericPop.gameCodePop,
// #codeInput, Submit / Leave). servers_ui.c shows it when a server refuses the join for its
// password (ERROR 004005).
void v10_party_password_prompt(void);
// Inline handlers of the popup: submitGameCode, knGameCodeLeave. -1 = not ours.
int v10_party_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc);
