#pragma once
// Privacy & Age (Krunker v10.0.4): the k-ID section of Account Settings, between Account Security and
// Account Privacy. Sources: deob-10.0.4.js:216008-216011 (the mount in the settings tab),
// deob-10.0.4.js:97480-97770 (the kid settings component: its gate, rows, switches, buttons, result line
// and the parent request dialog) and deob-10.0.4.js:26519-26543 (the kid API on the gapi: GET /account/kid,
// POST /account/kid/permissions, POST /account/kid/permissions/email).
#include <html_dom.h>
#include <ui_logic.h>

// GET /account/kid for the signed-in krunker.io account when the tab mounts (v10's onMount refresh, 97617):
// an answer younger than 15 s is reused (97539-97541). on_change runs whenever the section changes (an answer,
// a request's result, the busy state), so the Account Settings tab can re-render.
void kid_settings_refresh(void (*on_change)(void));

// Appends the section. Nothing is appended while there is no k-ID session (v10's gate: `status !== "none"`).
void kid_settings_html(StrBuf *sb);

// Forgets the answer, the result line and the 30 s poll (sign-out): the next account must not see this one's.
void kid_settings_clear(void);

// The section's inline handlers: knKidRequest('<permission>') (a switch / Ask a Parent), knKidPending()
// (Show Code / Verify Age), knKidSend() and knKidCopy() (the parent request dialog). -1 = not ours; else
// the handler result (0 cancels the default action: the switch is not toggled locally, 97497-97500).
int kid_settings_handle(const char *name, const HtmlArg *args, int argc);
