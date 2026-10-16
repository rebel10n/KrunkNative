#pragma once
// The bhop toast: the top-right message that slides in when the bhop mode changes ("Bhop -
// Slidehop (while moving)") and when Ctrl TP is toggled ("Ctrl TP - Enabled"). It is the toast
// of the v3.1.1 offline build's client mod (Krunker_v3.1.1/mod/notify.ts), as ui_notify.c drew
// it before the notification centre (commit e333da9c replaced it), restored for these two.
//
// TWO CALLERS, AND NO OTHERS. ui_bhop.c (the bhop mode message: the toggle key and /bhop) and
// ui_mod_keys.c (the Ctrl TP toggle message, the one exception to bhop-only that the user asked
// for). Generic system messages (reloads, server notices, settings feedback) must not use this
// toast: they go through ui_notify() (ui_notify.c) and the v10 notification centre, never
// through this module, and these messages never go through ui_notify(). Enforced by `make lint`:
// only ui_bhop.c and ui_mod_keys.c may call bhop_toast_show(), only hud_ui.c may tick it, and
// nothing else names its element or includes its header.
#include <stddef.h>

// The one entry point: show `text` ("Bhop - <mode name>") in the bhop toast, restarting its slide.
void bhop_toast_show(const char *text);

// Per frame (hud_ui.c): steps the slide. Not a display call; nothing else drives the toast.
void bhop_toast_tick(double now_ms);
