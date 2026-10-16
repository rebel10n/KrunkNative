#pragma once
// Krunker Strike's notifications holder (FRVR 1.37.2 _ue / OI / Ag, bundle 98829-98884; the
// .frvr-notifications-holder above the screens): three notifications on screen at most, the rest
// queued in order. A notification leaves on its close button, on its CTA, or when its timer runs out
// (15 s, FRVR's inviteNotificationParams().timer default), and then the first queued one takes its
// place with a timer of its own.
//
//   client/src/ui/strike_notify_queue.c   the queue (no HTML: strike_notify_test links it alone)
//   client/src/ui/strike_notify.c         the holder's markup (N5, the game invite view) and actions
//
// The quest toast (zB) needs Discord missions, which Krunker Strike does not have, so it is not here.
#include <stddef.h>
#include <cJSON.h>

#define STRIKE_NOTIFY_MAX 3            // OI: da.length < 3
#define STRIKE_NOTIFY_QUEUE_MAX 16     // FRVR's GC is unbounded; a full queue drops the newest
#define STRIKE_NOTIFY_TIMER_S 15.0     // inviteNotificationParams().timer ?? 15

typedef enum {
    STRIKE_NOTIFY_INVITE = 0,          // N5: "<name> invited you to join a match." [Join]
} StrikeNotifyKind;

typedef struct {
    int key;                           // a stable id (FRVR's id is the list index, which moves)
    StrikeNotifyKind kind;
    char name[40];                     // the friend's name (N5 falls back to "Player Name ")
    char game_id[48];                  // the match the Join button asks for
    double expires_s;                  // the timer's end (shown items only)
} StrikeNotify;

typedef struct {
    StrikeNotify shown[STRIKE_NOTIFY_MAX];
    int shown_count;
    StrikeNotify queued[STRIKE_NOTIFY_QUEUE_MAX];
    int queued_count;
    int next_key;
    unsigned version;                  // bumped on every change (the holder redraws when it moved)
} StrikeNotifyQueue;

// The queue (strike_notify_queue.c). Times are seconds on any monotonic clock.
void strike_notify_queue_init(StrikeNotifyQueue *q);
// Shown when fewer than three are up, else queued. Returns the key, 0 when the queue is full.
int  strike_notify_queue_push(StrikeNotifyQueue *q, StrikeNotifyKind kind, const char *name, const char *game_id, double now_s);
// Takes a shown notification off (its close button or CTA): the first queued one is shown in its place.
// Returns 1 when a shown one left; queued ones are not on screen and cannot be dismissed (0).
int  strike_notify_queue_dismiss(StrikeNotifyQueue *q, int key, double now_s);
// Timers: every shown notification whose timer ran out leaves (auto dismiss).
void strike_notify_queue_tick(StrikeNotifyQueue *q, double now_s);

// The holder (strike_notify.c): pushes into the page's queue, draws the holder and runs the actions
// the markup can call (notify-dismiss:<key>, notify-cta:<key>; strike_menu.c's action() forwards them).
void strike_notify_push_invite(const char *name, const char *game_id); // no-op outside Krunker Strike's page
void strike_notify_tick(void);                                         // per frame (strike_ui_tick)
int  strike_notify_action(const char *name);                           // 1 = one of ours
void strike_notify_scenario(const cJSON *list);                        // state.strike.notifications: [{name, gameId}]
