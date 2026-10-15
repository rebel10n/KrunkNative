#pragma once
// Profile pictures on the account page (kn_brain: POST /api/account/avatar, GET /api/avatars/:id;
// services/brain/avatar.c). The player picks a PNG, JPEG or WebP; the file is sent to the brain,
// which re-encodes it and has it reviewed by the AI moderation model. The picture is shown to
// others only once approved; the account page shows the review's status (never the image).
//
// The file dialog: native uses the desktop's zenity / kdialog (as the editor's ed_pick_file does),
// the web build an <input type=file> through EM_JS (client/src/ui/avatar_ui.c).

// "Change Picture" on the account page (the named handler knPfpPick): opens the picker.
void avatar_ui_pick(void);
// Asks the brain for the signed-in account's picture status and writes it into the account
// page's status line (#pfpStatus). A no-op without a brain or a session.
void avatar_ui_refresh(void);
