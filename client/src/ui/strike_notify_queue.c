// Krunker Strike's notification queue (client/include/strike_notify.h): FRVR 1.37.2's OI (push: shown
// while da.length < 3, else GC.push) and Ag (a shown one leaves: its timer is cleared, the list
// splices, then while fewer than three are shown the first queued one is shown with a new timer),
// and the auto dismiss of notification_auto_dismiss. No HTML here, so strike_notify_test links it alone.
#include <strike_notify.h>
#include <string.h>

void strike_notify_queue_init(StrikeNotifyQueue *q) {
    memset(q, 0, sizeof(*q));
    q->next_key = 1;
}

static void copy_text(char *dst, size_t n, const char *src) {
    if (!src) src = "";
    strncpy(dst, src, n - 1);
    dst[n - 1] = 0;
}

// OI's timer: (inviteNotificationParams().timer ?? 15) * 1000 ms, from when it is shown
static void show(StrikeNotifyQueue *q, const StrikeNotify *n, const double now_s) {
    q->shown[q->shown_count] = *n;
    q->shown[q->shown_count].expires_s = now_s + STRIKE_NOTIFY_TIMER_S;
    q->shown_count++;
    q->version++;
}

int strike_notify_queue_push(StrikeNotifyQueue *q, const StrikeNotifyKind kind, const char *name, const char *game_id,
                             const double now_s) {
    StrikeNotify n;
    memset(&n, 0, sizeof(n));
    n.key = q->next_key;
    n.kind = kind;
    copy_text(n.name, sizeof(n.name), name && name[0] ? name : "Player Name ");
    copy_text(n.game_id, sizeof(n.game_id), game_id);
    if (q->shown_count < STRIKE_NOTIFY_MAX) {
        show(q, &n, now_s);
    } else if (q->queued_count < STRIKE_NOTIFY_QUEUE_MAX) {
        q->queued[q->queued_count++] = n;
        q->version++;
    } else {
        return 0;
    }
    q->next_key++;
    return n.key;
}

int strike_notify_queue_dismiss(StrikeNotifyQueue *q, const int key, const double now_s) {
    int at = -1;
    for (int i = 0; i < q->shown_count; i++) {
        if (q->shown[i].key == key) {
            at = i;
            break;
        }
    }
    if (at < 0) return 0;
    // splice, keeping the order of the rest (FRVR's da.splice(e, 1))
    for (int i = at; i + 1 < q->shown_count; i++) q->shown[i] = q->shown[i + 1];
    q->shown_count--;
    q->version++;
    // Ag: while fewer than three are shown, the first queued one comes up (its timer starts now)
    while (q->shown_count < STRIKE_NOTIFY_MAX && q->queued_count > 0) {
        const StrikeNotify next = q->queued[0];
        for (int i = 0; i + 1 < q->queued_count; i++) q->queued[i] = q->queued[i + 1];
        q->queued_count--;
        show(q, &next, now_s);
    }
    return 1;
}

void strike_notify_queue_tick(StrikeNotifyQueue *q, const double now_s) {
    // a dismissal can bring a queued one up, whose timer starts now, so the scan restarts from the top
    for (int i = 0; i < q->shown_count;) {
        if (q->shown[i].expires_s <= now_s) {
            strike_notify_queue_dismiss(q, q->shown[i].key, now_s);
            i = 0;
        } else {
            i++;
        }
    }
}
