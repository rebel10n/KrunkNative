#pragma once
// The account split (doc/v10/account-split.md): a krunker.io login is only ever used on an official krunker.io game,
// a Box Office (krunk.io) login only on a Box Office server. This header is the one predicate every path that could
// carry one side's identity or token to the other asks (client/src/ui/account_split.c).
//
// The card's side is the connected server's backend (backend_ui.h account_backend()): the login card, Use in the
// Account Manager and the Live Streams / ranked / social reads all follow it, never the sign-in state alone.
#include <backend_ui.h> // AccountBackend

// The side a login name belongs to, as this client knows it: the signed-in krunker.io account or a krunker.io row of
// the Account Manager (ACCOUNT_BACKEND_KRUNKER), the signed-in Box Office account or a Box Office row
// (ACCOUNT_BACKEND_KRUNKIO). Case-blind, as both services' logins are. A name the client knows on neither side is
// unknown: -1 (the server decides, by the password). A name known on both sides is not refused by either card.
int account_split_side_of(const char *login);

// 1 when a login typed on the card of the connected server's backend must not be sent: the name is known to be an
// account of the OTHER backend and not of this one. Box Office's card never sends it to kn_brain; the krunker.io card
// never signs it in.
int account_split_refuses(const char *login);

// 1 when an Account Manager row may be used on the connected server: its kind (krunker_row: 1 = krunker.io, 0 = Box
// Office) is the connected server's backend. Use of any other row opens no card and signs nobody in.
int account_split_row_usable(int krunker_row);

// 1 while a krunker.io account is signed in AND an official krunker.io game is the session (the connected server's
// backend). This is the only krunker.io identity a request may carry: its Bearer token, its krunker.io source choice
// (Live Streams), its ranked route, its event bus. On a Box Office server a signed-in krunker.io account supplies
// nothing and is shown as nobody's.
int account_split_krunker_identity(void);

// 1 while the connected server's backend has a signed-in account this client acts as: the krunker.io account in an
// official krunker.io game, the Box Office session on a Box Office server (its session token). The one sign-in test for
// a window's gate (the editor's AI panel, the hub's login popup). A Box Office session is no sign-in on krunker.io.
int account_split_signed_in(void);
