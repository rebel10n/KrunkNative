# Account Manager: the account list

The Account Manager window (`ALT_MANAGER_WINDOW`, `client/src/ui/alt_manager_ui.c`) lists the accounts the
player added, both backends in one list. Each row is one account: its name, a tag (`krunk.io` for a
Box Office account, `krunker.io` for an official account), Use and Remove. The official krunker.io account
is the same account the game signs in to; the window only lists and switches it.

## Decisions

1. **The list is the alt vault.** The rows live in the sealed vault (`$XDG_CONFIG_HOME/krunknative/alt/vault.bin`,
   AES-256-GCM, `client/src/alt/`), the client's own file. Nothing is written in plain text.
   - krunk.io row: the login name, its session token, and its password (`pass`, sealed with the rest).
   - krunker.io row: the login name, its refresh token and its password (`ALT_KIND_KRUNKER`).
2. **Only listed accounts are kept.** A sign-in is not stored by itself. Add lists the account; a later
   sign-in (the login popup, register, reset) refreshes a listed account's token only. An account is kept
   across restarts only when it is in the list, and only while it has a session or a password: an entry with
   neither is dropped when the vault opens (`alt_vault_prune`).
3. **Add** takes a login and a password typed in the window, and the chip above the fields picks the
   backend (`krunk.io` / `krunker.io`). It signs the typed account in only to fetch its token, lists it with
   the token and the password, and says `Added <name>.`. **It does not switch**: the signed-in account (or the
   guest) stays as it is, and the new row is not active. krunk.io goes through account_ui.c's `loginAcc`
   request (AM_ADD: `alt_store_list`); krunker.io through `krunker_account_add`, which hands the new refresh
   token to the listener (KRUNKER_EV_ADDED) and leaves the game's session untouched. When krunker.io asks for
   a code, the window shows a code field and submits it through `krunker_account_submit_code`.
4. **Use** signs the row in with its stored session: krunk.io through `account_ui_use_session`, krunker.io
   through `krunker_account_resume` (the refresh token, FRVR's refresh). When the session is refused (or
   missing), the stored password signs the account in again, silently (`sign_in_with_password`,
   `krunker_account_sign_in_when_expired`): no prompt. The refused session is forgotten; the row stays.
   When the password is refused too, the stored password is forgotten, the row stays listed for this run and
   says `Saved login is no longer valid. Sign in again to restore it.` (`kn.alt.invalid`); its Sign in button
   fills the form with that login and the chip. The window never asks for a password for a saved row. A
   row with neither a session nor a password (after such a refusal) has no Use: it offers Sign in.
5. **Remove** forgets the row and its stored session, and signs the account out when it is the one
   signed in (krunker.io: `krunker_account_sign_out`).
6. **Logout and switching.** krunk.io's logout revokes the token on the server, so a listed krunk.io account
   asks for its password after Logout. krunker.io's sign-out is local (FRVR), so its stored refresh token
   still works.
7. **Official games.** Add, Use and a krunker.io Remove are refused while an official game is played
   (the game is on the krunker.io account). The header's guest Login (v10's login modal) stays for an
   official guest.
8. **Names.** krunker.io logins are case-blind: a row matches its account whatever the case the server
   reports after a restart.
9. **Passwords and codes** are typed into the window, sent once, wiped from the client's buffers. They are
   never stored and never logged. Tokens are never logged.
10. **Legacy keys.** The v3.1.1 keys (`krunker_token`, `krunker_username`, `krunker_id`) are read once at
    startup, moved into the list when the vault can be written, and removed.
11. **No cipher / no vault.** Accounts cannot be listed and nothing is persisted. Sign-in works for the run.
12. **Labels only.** The window shows the list, the tags, Use / Remove, the form fields, Add, the code field
    and Submit, the krunker.io Login for an official guest, and one status line.

The krunker.io account's own session (its `kio_*` keys) is unchanged: it still restores at startup as before.

## Layout

Every row is the same CSS grid (`ui/css/kn-alt.css`): name (flexible, ellipsis) | tag (84 px) | Use (72 px) |
Remove (84 px). The active row shows its Use dimmed and an inset outline, which does not shift the columns.
`tools/uidiff/scenarios/account-manager-rows.json` renders two rows, one per backend (`--render-scenario`);
`state.altAccounts` stands in for the vault's rows.

## Code map

- `client/src/alt/alt_vault.c`: `alt_vault_clear_secret` (the row kept, its token wiped).
- `client/src/alt/alt_store.c`: `alt_store_kn_listed`, `alt_store_kn_forget`; `alt_store_kn_session` returns
  nothing for a row without a token.
- `client/src/net/krunker_account.c`: `krunker_account_resume`, `krunker_account_refresh_token`,
  `krunker_account_listen_events` (signed in, failed, resumed, refresh refused, code asked, refreshed).
- `client/src/ui/account_ui.c`: `persist_session`, `load_session`, `account_response` (reports to the window),
  `account_ui_use_session`, `account_ui_add_sign_in`, `account_ui_sign_out_of`, `account_saved_name`.
- `client/src/ui/alt_manager_ui.c`: the rows (both kinds), Add / Use / Remove, the krunker.io events, the code step.

## Tests

- `client/tests/alt_vault_test.c` (`make alt-test`): the vault's forget / listed cases, and the password checks
  (an Add lists without switching, the password is sealed, a forgotten session keeps it, a restart drops an
  entry with neither).
- `client/tests/account_manager_e2e.py` (`make account-manager-test`): krunk.io: register two accounts through
  the popup, Add both (no account switched, the current one kept), Use with the stored token, Add while another
  is signed in (listed, no switch) and Use after it, restart and Use with no prompt, a refused token signs in
  with the stored password (no prompt), a refused token and password say the login is no longer valid and
  offer Sign in, a restart drops the row with neither. krunker.io: Add with the captcha and the 2FA code (no
  sign-in), Use with the stored refresh token, a refused refresh signs in with the stored password (2FA code
  typed, no prompt), Remove and sign out. Checks that no password or token is on a client file or in a log.
- `client/tests/native_auth_e2e.py`, `server/tests/account_unify_e2e.py`: a plain sign-in is not listed; the
  krunker.io guest keeps the header's Login.
- `server/tests/krunker_account_e2e.py`: its stub takes a `refresh_ok` override (the expired-session case).

## Not done

- Enter does not submit the Add form; the Add button does.
- No "save the current account" button; Add lists the typed account without switching to it (Use switches).
