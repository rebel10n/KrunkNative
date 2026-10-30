#pragma once
// Desktop (OS) notifications for the notification centre's items (client/src/ui/desktop_notify.c).
//
// The items that also go to the desktop are the ones a player would otherwise miss while in a
// game or away from the chat: friend requests and friend additions (v10's "Followers"), direct
// messages that are not on screen, Wild Offers and surveys. Each source calls desktop_notify_item()
// once, next to its v10_notify push, so an item reaches the desktop exactly once. System messages,
// trades, gifts and the ranked queue stay in the centre only.
//
// Backends: Linux posts through notify-send (libnotify-bin) when it is on PATH, macOS through
// osascript (display notification), Windows and the web build have none (desktop_notify_backend()
// is "none"). No library is linked. Posting is skipped in headless runs and test runs
// (KRUNKNATIVE_HEADLESS, or --headless which sets it; KRUNKNATIVE_NO_BROWSER), so a test never
// shows a notification on the user's desktop.
//
// The switch (the menu's Notifications toggle, v10_menu.c) is stored in the client settings
// (knClient "desktopNotify", default on; client_settings.h) and copied here by the settings module.
#include <stddef.h>

// The switch. Default on.
void desktop_notify_set_enabled(int on);
int desktop_notify_enabled(void);
// The stored desktopNotify value ("true" / "false", NULL when absent): 0 only for "false".
int desktop_notify_parse(const char *stored);

// A desktop notifier exists on this system (desktop_notify_backend() is not "none").
int desktop_notify_available(void);
// "notify-send", "osascript" or "none".
const char *desktop_notify_backend(void);
// The switch's caption under the toggle, for the state (switch on/off, notifier present).
void desktop_notify_caption(int enabled, int available, char *out, size_t n);

// One centre item was pushed: posts it to the desktop when the switch is on. Titles and texts are
// plain text. Returns nothing; a failed post is silent (the centre still shows the item).
void desktop_notify_item(const char *title, const char *text);

// Test seam: when set, an enabled item goes to `sink` instead of the desktop (no process is started).
typedef void (*DesktopNotifySink)(const char *title, const char *text);
void desktop_notify_set_sink(DesktopNotifySink sink);
