#pragma once
// The Account Manager window (ALT_MANAGER_WINDOW = KN_WINDOW_BASE + 4, ui_logic.h): the accounts the player
// listed, krunk.io (Box Office) and krunker.io in one list. Each row shows its name, a backend tag, Use and
// Remove; Add signs in with a typed login and password. Opened from the header's Account Manager item (v10_menu.c).
#include <cJSON.h>
#include <html_dom.h>

// What account_ui.c reports for an Add or a switch the window started:
//   OK       signed in; the account is listed (its session stored)
//   REFUSED  the server said no (the error text) or never answered
//   EXPIRED  a switch's saved session was refused: the account asks for its password
//   UNSAVED  signed in, but the vault could not be written
typedef enum {
    ALT_SIGN_IN_OK,
    ALT_SIGN_IN_REFUSED,
    ALT_SIGN_IN_EXPIRED,
    ALT_SIGN_IN_UNSAVED,
} AltSignIn;
void alt_manager_ui_sign_in_done(AltSignIn result, const char *name, const char *error); // error NULL = none

// Once at startup (account_ui_init): the krunker.io account's events (krunker_account.h).
void alt_manager_ui_init(void);
// --render-scenario: state.altAccounts [{name, backend: "krunk.io" | "krunker.io", active}] stands in for the
// vault's rows (NULL = the vault again).
void alt_manager_ui_scenario(const cJSON *rows);

// The window's markup (NULL for other ids). Opens the vault if the player has one.
char *alt_manager_window_gen(int id, const char **header);
// knAlt* handlers from the window (-1 = not ours).
int alt_manager_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc);
