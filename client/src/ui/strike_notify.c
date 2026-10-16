// Krunker Strike's notifications on the page (client/include/strike_notify.h): the queue of
// strike_notify_queue.c drawn into .frvr-notifications-holder as FRVR's game invite view (N5,
// bundle 109980: .game-invite-notification with the close button, the timer ring, the text and Join).
// The holder is redrawn when the queue changed (its version), on each frame and after a push.
//
// Join (N5's b5(gameId)) has no route on this page yet: the friend invites that would push these
// (strike-screens-2) bring the join. Until then Join takes the notification away, like FRVR's CTA does.
#include <strike_notify.h>
#include <strike_ui.h>
#include <html_dom.h>
#include <ui_logic.h> // StrBuf, sb_append_esc
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static StrikeNotifyQueue Q;
static int g_queue_ready;
static unsigned g_drawn = (unsigned) -1; // the version the holder shows

static void queue_ready(void) {
    if (g_queue_ready) return;
    strike_notify_queue_init(&Q);
    g_queue_ready = 1;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec * 1e-9;
}

static HtmlElem holder(void) {
    const HtmlElem page = html_dom_by_id("frvrMenuMobileHolder");
    return page ? html_dom_query(page, ".frvr-notifications-holder") : 0;
}

// N5's view for each shown notification, in the queue's order (_ue's list)
static void draw(void) {
    const HtmlElem h = holder();
    if (!h) return;
    StrBuf sb = {0};
    for (int i = 0; i < Q.shown_count; i++) {
        const StrikeNotify *n = &Q.shown[i];
        char buf[160];
        snprintf(buf, sizeof(buf), "<div class=\"game-invite-notification\" id=\"frvr-notification_%d\">", n->key);
        sb_append(&sb, buf);
        snprintf(buf, sizeof(buf), "<div class=\"game-invite-notification__close\" onclick=\"strikeAction('notify-dismiss:%d')\">", n->key);
        sb_append(&sb, buf);
        sb_append(&sb, "<img src=\"./img/frvr-ui/close-symbol.svg\"><svg style=\"--invite-timer:15s\"><circle></circle></svg></div>");
        sb_append(&sb, "<div class=\"game-invite-notification__text\"><span class=\"game-invite-notification__text--blue\">");
        sb_append_esc(&sb, n->name);
        sb_append(&sb, "</span> invited you to join a match.</div>");
        snprintf(buf, sizeof(buf), "<div class=\"frvr-button frvr-button--blue frvr-button--size-xxs join_game\" onclick=\"strikeAction('notify-cta:%d')\">"
                 "<span class=\"frvr-button-text\">Join</span></div>", n->key);
        sb_append(&sb, buf);
        sb_append(&sb, "</div>");
    }
    char *html = sb_finish(&sb);
    html_dom_set_html(h, html ? html : "");
    free(html);
    g_drawn = Q.version;
}

void strike_notify_push_invite(const char *name, const char *game_id) {
    if (!strike_ui_active()) return;
    queue_ready();
    strike_notify_queue_push(&Q, STRIKE_NOTIFY_INVITE, name, game_id, now_s());
    draw();
}

void strike_notify_tick(void) {
    if (!strike_ui_active() || !g_queue_ready) return;
    strike_notify_queue_tick(&Q, now_s());
    if (Q.version != g_drawn) draw();
}

// notify-dismiss:<key> (the close button) and notify-cta:<key> (Join): the notification leaves
int strike_notify_action(const char *name) {
    int key = 0;
    if (!g_queue_ready) return 0;
    if (sscanf(name, "notify-dismiss:%d", &key) == 1 || sscanf(name, "notify-cta:%d", &key) == 1) {
        const int cta = strncmp(name, "notify-cta:", 11) == 0;
        for (int i = 0; i < Q.shown_count; i++) {
            if (Q.shown[i].key != key) continue;
            if (cta) printf("[strike] notification %d: join %s (no route yet)\n", key, Q.shown[i].game_id);
            break;
        }
        strike_notify_queue_dismiss(&Q, key, now_s());
        if (Q.version != g_drawn) draw();
        return 1;
    }
    return 0;
}

// state.strike.notifications (--render-scenario): the pushes, with the holder drawn at once
void strike_notify_scenario(const cJSON *list) {
    if (!list || !cJSON_IsArray(list) || !strike_ui_active()) return;
    queue_ready();
    const cJSON *item;
    cJSON_ArrayForEach(item, list) {
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "name"));
        const char *game = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "gameId"));
        strike_notify_queue_push(&Q, STRIKE_NOTIFY_INVITE, name, game, now_s());
    }
    draw();
}
