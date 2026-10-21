# Account Manager: the account list

The Account Manager window (`ALT_MANAGER_WINDOW`, `client/src/ui/alt_manager_ui.c`) keeps track of the accounts
the player listed, both backends in one list, so that their logins can be filled in later. Each row is one
account: its name, a tag (`krunk.io` for a Box Office account, `krunker.io` for an official account), Use and
Remove. The window never signs anyone in: the player presses Login on the login card.

## Decisions

1. **The list is the alt vault.** The rows live in the sealed vault (`$XDG_CONFIG_HOME/krunknative/alt/vault.bin`,
   AES-256-GCM, `client/src/alt/`), the client's own file. Nothing is written in plain text.
   - krunk.io row: the login name, its password (`pass`), and its session token once the player has signed in.
   - krunker.io row: the login name, its password, and its refresh token once the player has signed in
     (`ALT_KIND_KRUNKER`).
2. **Only listed accounts are kept.** A sign-in through the login card stores its session only under a listed
   account (`persist_session`, `sync_kio`). A row is kept across restarts while it has a password or a session;
   an entry with neither is dropped when the vault opens (`alt_vault_prune`).
3. **Add** takes a login and a password typed in the window; the chip above the fields picks the backend
   (`krunk.io` / `krunker.io`). It stores the login, the password and the kind (`alt_store_list`, no token) and
   lists the row: `Added <name>.` **Nothing is signed in, nothing is switched, and no request leaves the client.**
   A krunker.io login that is already listed (case-blind) keeps its row.
4. **Use** opens the login card (`v10_login_fill`, `client/src/ui/v10_login.c`) with the row's login in Username
   and its saved password in Password, and stops. No sign-in request, no session restore, no password prompt.
   The player presses Login, which signs in to the card's backend: a krunker.io row's card signs in to
   krunker.io, a krunk.io row's to krunk.io's account service. **Use never refuses**: over a signed-in account
   (whoever is signed in) and in an official game the card opens with the row's login all the same. Login is the
   player's press; the window shows no refusal text for Use.
   **Login on a card over a signed-in account** (either backend) signs in only when the login typed is the signed-in
   account's: a name is compared with the signed-in account's name (case-blind), and an address, typed in either field,
   with the address on file (the brain pushes it after every login, `emailS`). Until that address is known the Login
   is refused, as a login that might be another account: nothing proves it is this one. Any other login sends
   nothing: the session is not switched and not signed out, and the card's message line says
   `Signed in as <name>. Log out to sign in as <login>.` (`kn.account.otherSignedIn`).
5. **Logout** ends the session as before (the krunk.io server revokes it, the row keeps its login). **Startup**
   resumes the listed active account from its stored session when the server accepts it. A refused session is
   forgotten and nothing is signed in: there is no silent sign-in with the stored password. The row stays, and
   Use fills its login.
6. **Remove** forgets the row and its stored session, and signs the account out when it is the one signed in
   (krunker.io: `krunker_account_sign_out`, outside an official game).
7. **Names.** krunker.io logins are case-blind: a row matches its account whatever the case the server reports.
8. **Passwords** are typed into the Add form, sealed in the vault on Add, and shown only in the login card on Use.
   They are never logged. Tokens are never logged.
9. **Legacy keys.** The v3.1.1 keys (`krunker_token`, `krunker_username`, `krunker_id`) are read once at
   startup, moved into the list when the vault can be written, and removed.
10. **No cipher / no vault.** Accounts cannot be listed: the window says so, and nothing is persisted.
11. **Labels only.** The window shows the list, the tags, Use / Remove, the Add form and the krunker.io Login
    for an official guest, and one status line.

The krunker.io account's own session (its `kio_*` keys) is unchanged: it still restores at startup as before.

## Layout

Every row is the same CSS grid (`ui/css/kn-alt.css`): name (flexible, ellipsis) | tag (84 px) | Use (72 px) |
Remove (84 px). The active row shows its Use dimmed and an inset outline, which does not shift the columns.
`tools/uidiff/scenarios/account-manager-rows.json` renders two rows, one per backend (`--render-scenario`);
`state.altAccounts` stands in for the vault's rows.

## Code map

- `client/src/alt/alt_vault.c`: `alt_vault_clear_secret` (the row kept, its session wiped).
- `client/src/alt/alt_store.c`: `alt_store_list` (Add), `alt_store_password`, `alt_store_kn_listed`,
  `alt_store_kn_forget`; `alt_store_kn_session` returns nothing for a row without a session.
- `client/src/ui/alt_manager_ui.c`: the rows (both kinds), Add / Use / Remove, the krunker.io events.
- `client/src/ui/v10_login.c`: the login card; `v10_login_fill` (Use's fill); `kio_form` picks the card's backend.
- `client/src/ui/account_ui.c`: `persist_session`, `load_session`, `try_restore`, `account_response`,
  `account_ui_sign_out_of` (Remove), `account_saved_name`.
- `client/src/net/krunker_account.c`: `krunker_account_sign_in`, `krunker_account_sign_out`,
  `krunker_account_refresh_token`, `krunker_account_listen_events`.

## Tests

- `client/tests/alt_vault_test.c` (`make alt-test`): the vault's listing, sealing, session forgetting and the restart
  prune (an entry with neither a session nor a password goes).
- `client/tests/account_manager_e2e.py` (`make account-manager-test`): register two krunk.io accounts through the
  popup; Add both with a --brain nothing listens on (listed, nothing signed in, no sign-in request); Use fills the
  card's login and password with no sign-in request; Use then Login signs in and stores the session; a restart
  resumes it with no prompt; Logout, then Use fills the card again with no sign-in; a refused stored session
  signs nobody in and Use still fills; Remove; Use while signed in opens the card with the row's login and shows no
  refusal; an official krunker.io game (a guest joined through the stub's matchmaker): Add a krunk.io login, Use
  fills the card, Remove; a krunker.io row: Add (no request to krunker.io), Use fills the card, its Login signs in to
  krunker.io (the Security Challenge and the stub's 2FA code) and the card closes, Remove. Checks that no password
  or token is on a client file or in a log.
- `client/tests/native_auth_e2e.py`, `server/tests/account_unify_e2e.py`: a plain sign-in is not listed; the
  krunker.io guest keeps the header's Login.

## Not done

- Enter does not submit the Add form; the Add button does.
