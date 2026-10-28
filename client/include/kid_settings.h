#pragma once
// Privacy & Age (Krunker v10.0.4): the k-ID section of Account Settings, between Account Security and
// Account Privacy. Sources: deob-10.0.4.js:216008-216011 (the mount in the settings tab),
// deob-10.0.4.js:97480-97760 (the kid settings component: its gate, rows and state labels) and
// deob-10.0.4.js:26519-26540 (the kid API on the gapi: GET /account/kid).
#include <ui_logic.h>

// GET /account/kid for the signed-in account, once per session (a failure is not retried until the next
// sign-in). on_change runs when the answer lands, so the Account Settings tab can re-render.
void kid_settings_refresh(void (*on_change)(void));

// Appends the section. Nothing is appended while there is no k-ID session (v10's gate: `status !== "none"`).
void kid_settings_html(StrBuf *sb);

// Forgets the answer (sign-out): the next account must not see this one's section.
void kid_settings_clear(void);
