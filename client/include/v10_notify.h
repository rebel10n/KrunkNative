#pragma once
// Krunker v10.0.2's boot-mounted Svelte widgets of the chat column (client/src/ui/v10_notify.c),
// mounted by v10 into #notificationCenter, #gameAnnouncementEl and #emojiPanelHolder
// (g:223110-223130):
//   - the notification center (svelte-nrigjp + the expiration bar svelte-16a8kon, g:197557-197640;
//     its store and push / dismiss g:118762-118787): newest first, slideIn 0.3 s, a countdown bar
//     refreshed every 100 ms, removed after `duration` (default 4000 ms) or when dismissed;
//   - the game announcement banner (svelte-1m80xxc, g:118323-118585; the GAME_ANNOUNCEMENT packet
//     g:210119), here fed by NET_EXT_GAME_ANNOUNCEMENT (server console `announce`);
//   - the chat's emoji panel (svelte-13g6jqo, g:197502-197556; emojiMap g:103080-103125, the
//     filter g:97404-97428, the chat keys g:202884-202913, selectEmoji g:221347) and the chat
//     lines' :name: -> <img> (g:221099-221112).
#include <html_dom.h>
#include <stddef.h>
#include <stdint.h>

#include <cJSON.h>

#define V10_NOTIFY_DEFAULT_MS 4000.0 // iíiìïïi: options.duration ?? 4000

// ---- notification center
// A click on a text notification (KrunkNative: v10's text ones have no control, its component
// ones a close button). The notification is dismissed after the callback.
typedef void (*V10NotifyClick)(void *user);
// A text notification (title + content, both plain text). duration_ms <= 0: no countdown bar and
// no expiry (v10: duration > 0 shows the bar and arms the removal). Returns its id (> 0).
int v10_notify_push(const char *title, const char *content, double duration_ms);
int v10_notify_push_ex(const char *title, const char *content, double duration_ms, V10NotifyClick on_click, void *user);
// A component notification (v10 iíiìïïi({title, content: Component, props})): `html` is the
// component's markup inside the .notification-item (it brings its own close control, which calls
// v10_notify_dismiss); title is for the log. Returns its id.
int v10_notify_push_html(const char *title, const char *html, double duration_ms);
void v10_notify_dismiss(int id); // iïïiïíï(id); unknown ids are ignored
void v10_notify_clear(void);
int v10_notify_count(void);
// 1 when the center can be seen now: its column (#chatUIHolder > #chatUI) is shown and v10's
// `hidden` rule (!$hasMenuUI && !$hasGameUI) is off. ui_notify()'s messages are not centre
// notifications (ui_notify.c draws them as the generic toast), so nothing queues for it.
int v10_notify_visible(void);

// ---- game announcement
// gameAnnouncement {message, ...}: shows the banner with the message (plain text); NULL / "" hides it.
void v10_announce_set(const char *message);
// NET_EXT_GAME_ANNOUNCEMENT body (shared/include/net_ext.h NetGameAnnouncementPacket)
void v10_announce_on_packet(const uint8_t *body, uint16_t length);

// ---- emoji
// The chat key while #chatInput has focus: with the panel open it picks the selected emoji
// (selectEmoji) instead of sending. 1 = used.
int v10_emoji_chat_key(void);
// Up (38) / Down (40) while #chatInput has focus and the panel is open: moves the selection
// (keydown, preventDefault). 1 = used.
int v10_emoji_arrow_key(int dom_key_code);
// Appends a chat message as the chat line shows it: UTILS.fixStr (escaped, wrapped in U+200E)
// with every :name: of the emoji map as the emoji's <img>. Bounded: an image that does not fit
// stays text.
void v10_emoji_append_chat(char *buf, size_t size, size_t *len, const char *msg);

// ---- frame + scenario
void v10_notify_tick(double now_ms); // every frame (ui_notify_tick): bars, expiry, the emoji panel
// --render-scenario state.notify: {notifications: [{title, text, duration, remainingPct}],
// announcement: "message", emoji: {query, selected}, chat: "text", tip: {...}} (ui_scenario.c)
void v10_notify_scenario(const cJSON *state);
// Inline handlers of the widgets (knNotifyClick, selectEmoji); -1 = not ours.
int v10_notify_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc);
