#pragma once
// The bhop toast: the top-right message that slides in when the bhop mode changes ("Bhop -
// Slidehop (while moving)"). It is the toast of the v3.1.1 offline build's client mod
// (Krunker_v3.1.1/mod/notify.ts), as ui_notify.c drew it before the notification centre
// (commit e333da9c replaced it), restored for bhop alone.
//
// BHOP-ONLY. This toast belongs to the bhop system (client/src/ui/ui_bhop.c) and must not be
// used for anything else, now or later. Generic system messages (reloads, server notices,
// settings feedback) go through ui_notify() (ui_notify.c), never through this module, and the
// bhop message never goes through ui_notify() or the v10 notification centre. Enforced by
// `make lint`: only ui_bhop.c may call bhop_toast_show(), and only hud_ui.c may tick it.
#include <stddef.h>

// The one entry point: show `text` ("Bhop - <mode name>") in the bhop toast, restarting its slide.
void bhop_toast_show(const char *text);

// Per frame (hud_ui.c): steps the slide. Not a display call; nothing else drives the toast.
void bhop_toast_tick(double now_ms);
