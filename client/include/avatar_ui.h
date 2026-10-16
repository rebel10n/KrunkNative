#pragma once
#include <stddef.h>
// Profile pictures on the account page (kn_brain: POST /api/account/avatar, GET /api/avatars/:id;
// services/brain/avatar.c). The player picks a PNG, JPEG or WebP; the file is sent to the brain,
// which re-encodes it and has it reviewed by the AI moderation model. The picture is shown to
// others only once approved; the account page shows the review's status (never the image).
//
// The file dialog: native uses the desktop's zenity / kdialog (as the editor's ed_pick_file does),
// the web build an <input type=file> through EM_JS (client/src/ui/avatar_ui.c). The clan logo
// (clan_ui.c) uses the same dialog.

// "Change Picture" on the account page (the named handler knPfpPick): opens the picker.
void avatar_ui_pick(void);
// The file dialog shared with the clan logo (windows[12].logoPick): the native zenity / kdialog
// chooser or the browser's file input. A file that passed the type and size checks goes to fn as
// base64 (bytes = its size before encoding), error NULL. error is "UnsupportedType" (not a PNG, or
// not a PNG / JPEG / WebP when png_only is 0), "FileTooLarge" (over max_bytes) or "BadImage" (could
// not be read); a cancelled dialog calls nothing. png_only: PNG only (the clan logo).
typedef void (*AvatarPickFn)(void *user, const char *b64, size_t bytes, const char *error);
void avatar_ui_pick_file(int png_only, size_t max_bytes, AvatarPickFn fn, void *user);
// Asks the brain for the signed-in account's picture status and writes it into the account
// page's status line (#pfpStatus). A no-op without a brain or a session.
void avatar_ui_refresh(void);
