// Krunker Strike's friends, invites and reports (strike_ui.h). FRVR runs these on its platform's
// social API (FRVR.social: the Discord activity's channel members, sendGameInvite, /v1/reports);
// here they run on kn_brain's social socket (backend_social_*): the friends event bus (mutual
// follows, presence, services/brain/social_eb.c), the friend's game with its flavor
// (game_presence.c: a friend in a Krunker Box Office game is not in a Strike game), game:invite
// and the hub's "rpt" report. Markup: ui/strike/screens/MENU_FRIENDS, INVITE_NOTIFICATION,
// REPORT_PLAYER, POPUP_ALERT.html, TRANSCRIBED from assets/index-Cz2uYXJQ.js (read as text):
//
//   S5 (ede / sde)  the menu friends: on TAP_TO_PLAY always (isInGame), on MAIN_MENU when the
//                   platform has friends (FRVR.social getFriends: here a signed-in account). ede is
//                   the four-bullet strip (online first, plus signs for the empty ones); a click
//                   hides it (50 ms) and opens sde, the Friends List: Hosting Match (no hosted
//                   matches here: empty), Playing Krunker (online; friends in one Strike game share
//                   a large bullet with the mode, "N Players" and Join, the others an x5 row with
//                   _5 Invite in a game), Offline; with none, "Gather your Krunker Crew". The dark
//                   blur / arrow close it (300 ms). Join = quickConnect of the friend's game
//                   (servers_ui_join_link); Invite = sendGameInvite (game:invite with this client's
//                   game: its private code or game id), "Invited" for 15 s.
//   Whe             MAIN_MENU's Private Match button (createMatchBtn, with friends: a signed-in account)
//                   opens CREATE_MATCH (Qhe: Pt(m5)).
//   Vhe / zhe       CREATE_MATCH's friend list (online first, state dots, Invite / Invited / Joined;
//                   FRVR sends the private match id before the host joins it: here an invite made
//                   before this client is in a lobby is sent once it is, with its private code).
//                   Join Match opens a private lobby (servers_ui_private_match: the director).
//   N5 INVITE_NOTIFICATION  OI(): up to 3 in .frvr-notifications-holder (more wait), 15 s each;
//                   Join joins the invite's game, x dismisses.
//   Hy REPORT_PLAYER        a1 prompt, reason dropdown (jl over xe.reportOptions) and comments
//                   (10+ characters); Report player validates, then ["rpt", alias, reason, info]
//                   -> "rptr": kr(XA) "Player reported successfully!" / kr(p0) the error. A guest
//                   (no account: the brain takes reports from accounts only) gets "Must be logged in".
// FRVR's profile pictures are the platform's; KrunkNative has none, so every friend shows
// getDefaultPFP(): the class icon 0. FRVR's "Invite Friends" / "+ More Friends" open the
// platform's invite dialog, which KrunkNative does not have (canInvite() false: no button).
#include <strike_ui.h>
#include "klog.h"
#include <html_ui.h>
#include <ui_logic.h>
#include <backend.h>
#include <backend_ui.h>
#include <flavor.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FRIENDS_MAX 200
#define NOTES_SHOWN 3   // OI: da.length < 3
#define NOTES_QUEUE 8
#define INVITE_MS 15000.0 // wi.inviteNotificationParams().timer default 15 s
#define PFP "./cdn/textures/classes/icon_0.png" // N.getDefaultPFP(): MT("textures/classes/icon_0.png")
#define MF "MENU_FRIENDS"

typedef struct {
    int64_t id;
    char name[40];
    int online;
    char game[96];   // a Strike game ("" = none, or a game of another flavor)
    char mode[48];
    int players;
    double invited_until; // _5: canInvite false for 15 s after an invite
    int match_invited;    // CREATE_MATCH: inviteState Invited
    int match_pending;    // invited before this client was in a lobby: sent once it is
} Friend;

typedef struct {
    int id;
    int64_t from;
    char name[40];
    char game[96];
    int timer;
} Note;

static const char *const REPORT_OPTIONS[] = {"Hacking", "Exploiting", "Boosting", "Offensive/Inappropriate Name", "Other"}; // xe.reportOptions
#define REPORT_OPTION_COUNT ((int) (sizeof(REPORT_OPTIONS) / sizeof(REPORT_OPTIONS[0])))

static struct {
    Client *client;
    int listening, requested, signed_in;
    Friend friends[FRIENDS_MAX];
    int count;
    int loading_offset; // the friends:list page being read (paging: hasMore)
    int dirty;
    int panel;          // vg: the Friends List is open instead of the strip
    int strip_hiding, panel_hiding;
    Note notes[NOTES_SHOWN];
    int note_count;
    Note queue[NOTES_QUEUE];
    int queue_count;
    int next_note;
    // REPORT_PLAYER (Hy viewState)
    int report_uid;
    char report_name[40];
    int reason;         // index into REPORT_OPTIONS, -1 = "Select"
    int reason_open;
    int should_validate;
    int sending;
    char alert[128];
} G = {.reason = -1};

static double now_ms(void) { // wall clock, like v10_social.c's
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double) ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static Friend *friend_by_id(const int64_t id) {
    for (int i = 0; i < G.count; i++) {
        if (G.friends[i].id == id) return &G.friends[i];
    }
    return NULL;
}

static const char *jstr(const cJSON *o, const char *k) {
    const char *s = o ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, k)) : NULL;
    return s ? s : "";
}

static double jnum(const cJSON *o, const char *k) {
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
    return cJSON_IsNumber(v) ? v->valuedouble : 0.0;
}

static void append_part(StrBuf *sb, const char *file, const char *part, const StrikeVar *vars, const int count) {
    char *html = strike_template_part(file, part, vars, count);
    if (html) {
        sb_append(sb, html);
        free(html);
    }
}

static char *part(const char *file, const char *name, const StrikeVar *vars, const int count) {
    char *html = strike_template_part(file, name, vars, count);
    return html ? html : calloc(1, 1);
}

// ---------------------------------------------------------------- the event bus

static void eb_send(cJSON *obj) {
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("eb"));
    cJSON_AddItemToArray(f, obj);
    backend_social_send(f);
    cJSON_Delete(f);
}

static void request_friends(const int offset) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "friends:list");
    cJSON_AddNumberToObject(o, "offset", offset);
    G.loading_offset = offset;
    eb_send(o);
}

// the friend's game, when it is a Strike one (gameFlavor: this client's flavor)
static void set_game(Friend *f, const cJSON *o) {
    const char *gid = jstr(o, "gameInstanceId"), *flavor = jstr(o, "gameFlavor");
    if (gid[0] && !strcmp(flavor, flavor_key(flavor_get()))) {
        snprintf(f->game, sizeof(f->game), "%s", gid);
        snprintf(f->mode, sizeof(f->mode), "%s", jstr(o, "gameMode"));
        f->players = (int) jnum(o, "gamePlayers");
    } else {
        f->game[0] = f->mode[0] = 0;
        f->players = 0;
    }
}

static void on_friends(const cJSON *o) {
    static Friend old[FRIENDS_MAX];
    static int old_count;
    const int offset = (int) jnum(o, "offset");
    if (offset == 0) { // a fresh list: this session's invite state stays with each friend
        memcpy(old, G.friends, sizeof(G.friends[0]) * (size_t) G.count);
        old_count = G.count;
        G.count = 0;
    }
    const cJSON *row;
    cJSON_ArrayForEach(row, cJSON_GetObjectItemCaseSensitive(o, "friends")) {
        const int64_t id = (int64_t) jnum(row, "id");
        if (id <= 0 || G.count >= FRIENDS_MAX) continue;
        Friend *f = &G.friends[G.count++];
        memset(f, 0, sizeof(*f));
        f->id = id;
        snprintf(f->name, sizeof(f->name), "%s", jstr(row, "name"));
        f->online = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(row, "online"));
        set_game(f, row);
        for (int i = 0; i < old_count; i++) {
            if (old[i].id != id) continue;
            f->invited_until = old[i].invited_until;
            f->match_invited = old[i].match_invited;
            f->match_pending = old[i].match_pending;
        }
    }
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "hasMore"))) request_friends(G.count);
    int online = 0;
    for (int i = 0; i < G.count; i++) online += G.friends[i].online;
    KN_LOG_VERBOSE2("[strike] friends: %d (%d online)\n", G.count, online);
    G.dirty = 1;
}

static void notes_render(void);
static void note_show(const Note *n);

static void on_invite(const cJSON *o) {
    Note n = {0};
    n.from = (int64_t) jnum(o, "from");
    snprintf(n.name, sizeof(n.name), "%s", jstr(o, "name"));
    snprintf(n.game, sizeof(n.game), "%s", jstr(o, "gameId"));
    if (!n.game[0]) return;
    KN_LOG_DEBUG1("[strike] invite from %s to %s\n", n.name, n.game);
    if (G.note_count < NOTES_SHOWN) note_show(&n);
    else if (G.queue_count < NOTES_QUEUE) G.queue[G.queue_count++] = n; // GC.push
}

static void report_result(int ok);

static void on_social(void *user, const cJSON *frame) {
    (void) user;
    const char *label = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 0));
    if (!label) return;
    if (!strcmp(label, "a")) { // a login answer: the friends of this account
        if (cJSON_IsNull(cJSON_GetArrayItem(frame, 1)) && active_account()) {
            G.signed_in = 1;
            request_friends(0);
        }
        return;
    }
    if (!strcmp(label, "rptr")) {
        if (G.sending) report_result(cJSON_IsTrue(cJSON_GetArrayItem(frame, 1)));
        return;
    }
    if (strcmp(label, "eb") != 0) return;
    const cJSON *o = cJSON_GetArrayItem(frame, 1);
    const char *type = jstr(o, "type");
    Friend *f = friend_by_id((int64_t) jnum(o, "id"));
    if (!strcmp(type, "friends:list")) on_friends(o);
    else if (!strcmp(type, "presence:online") && f) f->online = 1, G.dirty = 1;
    else if (!strcmp(type, "presence:offline") && f) f->online = 0, f->game[0] = 0, G.dirty = 1;
    else if (!strcmp(type, "presence:game") && f) set_game(f, o), G.dirty = 1;
    else if (!strcmp(type, "game:invite")) on_invite(o);
    else if (!strcmp(type, "game:invite:sent")) KN_LOG_DEBUG1("[strike] invite sent to %lld (%s)\n", (long long) jnum(o, "to"), jstr(o, "gameId"));
    else if (!strcmp(type, "error")) KN_LOG_VERBOSE2("[strike] friends: %s %s\n", jstr(o, "code"), jstr(o, "message"));
}

static void send_invite(Friend *f, const char *game) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "game:invite");
    cJSON_AddNumberToObject(o, "to", (double) f->id);
    cJSON_AddStringToObject(o, "gameId", game);
    eb_send(o);
    KN_LOG_DEBUG1("[strike] invite %s to %s\n", f->name, game);
}

// ---------------------------------------------------------------- S5: ede / sde

// the friends in FRVR's order: online first (Array.sort is stable)
static int sorted(const Friend **out) {
    int n = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < G.count; i++) {
            if (G.friends[i].online == !pass) out[n++] = &G.friends[i];
        }
    }
    return n;
}

static char *strip_html(void) {
    const Friend *list[FRIENDS_MAX];
    const int n = sorted(list);
    StrBuf bullets = {0};
    for (int i = 0; i < 4; i++) { // padded with {} to 4, first 4
        if (i < n) {
            char *dot = list[i]->online ? part(MF, "online-dot", NULL, 0) : NULL;
            const StrikeVar v[] = {{"pfp", PFP}, {"dot", dot ? dot : ""}};
            append_part(&bullets, MF, "bullet", v, 2);
            free(dot);
        } else {
            append_part(&bullets, MF, "bullet-empty", NULL, 0);
        }
    }
    char *b = sb_finish(&bullets);
    const StrikeVar v[] = {{"hide", G.strip_hiding ? " menu-friends--hide" : ""}, {"bullets", b ? b : ""}};
    char *html = strike_template(MF, v, 2);
    free(b);
    return html;
}

static char *invite_button(const Friend *f) {
    char id[24];
    snprintf(id, sizeof(id), "%lld", (long long) f->id);
    const StrikeVar v[] = {{"id", id}};
    return part(MF, f->invited_until > now_ms() ? "invited" : "invite", v, 1);
}

static char *member_html(const char *which, const Friend *f, const int with_invite) {
    char *invite = with_invite ? invite_button(f) : NULL;
    const StrikeVar v[] = {{"status", f->online ? "menu-friends-list__section-status menu-friends-list__section-status--online"
                                                : "menu-friends-list__section-status"},
                           {"pfp", PFP}, {"name", f->name}, {"invite", invite ? invite : ""}};
    char *html = part(MF, which, v, 4);
    free(invite);
    return html;
}

// rF: friends in one game share a large bullet (mode, players, Join); the others an x5 row
static char *section_html(const char *title, const Friend **list, const int n, const int in_game, const int offline) {
    StrBuf rows = {0};
    char seen[FRIENDS_MAX] = {0};
    for (int i = 0; i < n; i++) {
        const Friend *o = list[i];
        if (seen[i]) continue;
        if (offline || !o->game[0]) { // x5 (nde rows have no isInGame)
            char *row = member_html("row", o, !offline && in_game);
            sb_append(&rows, row);
            free(row);
            continue;
        }
        const int mine = servers_ui_is_current_game(o->game);
        const int join = !in_game || !mine;  // tde
        const int invite = in_game && !mine; // ide
        StrBuf members = {0};
        int d = 0;
        for (int j = i; j < n; j++) {
            if (seen[j] || strcmp(list[j]->game, o->game) != 0) continue;
            seen[j] = 1;
            d++;
            char *m = member_html("member", list[j], invite);
            sb_append(&members, m);
            free(m);
        }
        const int f = o->players > d ? o->players : d;
        char count[48], id[24];
        snprintf(count, sizeof(count), "%d %s", f, f > 1 ? "Players" : "Player");
        snprintf(id, sizeof(id), "%lld", (long long) o->id);
        const StrikeVar jv[] = {{"id", id}};
        char *jb = join ? part(MF, "join", jv, 1) : NULL;
        char *mh = sb_finish(&members);
        const StrikeVar v[] = {{"members", mh ? mh : ""}, {"mode", o->mode}, {"count", count}, {"join", jb ? jb : ""}};
        append_part(&rows, MF, "game", v, 4);
        free(mh);
        free(jb);
    }
    char *r = sb_finish(&rows);
    const StrikeVar v[] = {{"title", title}, {"rows", r ? r : ""}};
    char *html = part(MF, "section", v, 2);
    free(r);
    return html;
}

static char *panel_html(const int in_game) {
    const Friend *online[FRIENDS_MAX], *offline[FRIENDS_MAX];
    int on = 0, off = 0;
    for (int i = 0; i < G.count; i++) {
        if (G.friends[i].online) online[on++] = &G.friends[i]; // o: online && !isHosting (no hosted matches here)
        else offline[off++] = &G.friends[i];
    }
    const int l = on + off;
    char *body;
    if (l) {
        StrBuf sections = {0};
        if (on) {
            char *s = section_html("Playing Krunker", online, on, in_game, 0);
            sb_append(&sections, s);
            free(s);
        }
        if (off) {
            char *s = section_html("Offline", offline, off, in_game, 1);
            sb_append(&sections, s);
            free(s);
        }
        char *sh = sb_finish(&sections);
        const StrikeVar v[] = {{"sections", sh ? sh : ""}};
        body = part(MF, "list-body", v, 1);
        free(sh);
    } else {
        body = part(MF, "no-friends", NULL, 0);
    }
    char *promo = l ? NULL : part(MF, "promo", NULL, 0);
    const StrikeVar v[] = {
        {"hideBlur", G.panel_hiding ? " menu-friends-list__dark-blur--hide" : ""},
        {"hideHolder", G.panel_hiding ? " menu-friends-list__holder--hide" : ""},
        {"title", l ? "Friends List" : "Gather your Krunker Crew"},
        {"body", body},
        {"btnHolder", l ? "menu-friends-list__btn-holder" : "menu-friends-list__btn-holder menu-friends-list__btn-holder--no-friends"},
        {"promo", promo ? promo : ""},
    };
    char *html = part(MF, "list", v, 6);
    free(body);
    free(promo);
    return html;
}

// S5 in a screen: replaces the strip / list that is there, else (MAIN_MENU) goes after the
// middle-left column, where FRVR's main menu view has it
static void friends_place(const HtmlElem root, const int in_game, const int shown) {
    HtmlElem old = html_dom_query(root, ".menu-friends, .menu-friends-list");
    if (!shown) {
        if (old) html_dom_remove(old);
        return;
    }
    char *html = G.panel ? panel_html(in_game) : strip_html();
    if (!html) return;
    if (old) {
        html_dom_insert_html(old, HTML_INSERT_BEFORE_BEGIN, html);
        html_dom_remove(old);
    } else {
        const HtmlElem anchor = html_dom_query(root, ".frvr-main-menu--middle-left");
        if (anchor) html_dom_insert_html(anchor, HTML_INSERT_AFTER_END, html);
    }
    free(html);
}

// Whe: MAIN_MENU's Private Match button (bottom right, first), when the platform has friends
static void private_match_place(const HtmlElem root, const int shown) {
    const HtmlElem old = html_dom_by_id("createMatchBtn");
    if (old && !shown) html_dom_remove(old);
    if (old || !shown) return;
    const HtmlElem right = html_dom_query(root, ".frvr-main-menu--bottom-right");
    char *html = right ? part(MF, "private-match", NULL, 0) : NULL;
    if (html) html_dom_insert_html(right, HTML_INSERT_AFTER_BEGIN, html);
    free(html);
}

// ---------------------------------------------------------------- CREATE_MATCH (Vhe / zhe)

static void match_render(const HtmlElem root) {
    const HtmlElem list = html_dom_query(root, ".frvr-create-match-invite-friends__friend-list");
    if (!list) return;
    const Friend *fr[FRIENDS_MAX];
    const int n = sorted(fr);
    StrBuf sb = {0};
    if (!n) append_part(&sb, MF, "match-empty", NULL, 0);
    for (int i = 0; i < n; i++) {
        const Friend *f = fr[i];
        char *button = NULL;
        if (f->online) {
            const int joined = f->game[0] && servers_ui_is_current_game(f->game);
            char id[24];
            snprintf(id, sizeof(id), "%lld", (long long) f->id);
            const StrikeVar bv[] = {{"id", id}, {"label", joined ? "Joined" : "Invited"}};
            button = part(MF, joined || f->match_invited ? "match-invite-done" : "match-invite", bv, 2);
        }
        const StrikeVar v[] = {{"state", f->online ? "frvr-create-match-invite-friends__friend-state frvr-create-match-invite-friends__friend-state--online"
                                                   : "frvr-create-match-invite-friends__friend-state"},
                               {"pfp", PFP}, {"name", f->name}, {"button", button ? button : ""}};
        append_part(&sb, MF, "match-row", v, 4);
        free(button);
    }
    char *html = sb_finish(&sb);
    html_dom_set_html(list, html ? html : "");
    free(html);
    // Join Match: quickConnect({serverId, isHosting}) -> a private lobby here
    const HtmlElem play = html_dom_by_id("playButton");
    if (play) html_dom_set_attr(play, "onclick", "strikeAction('create-match-join')");
}

// ---------------------------------------------------------------- N5 notifications (OI / Ag)

static void note_expire(void *user);

static void notes_render(void) {
    const HtmlElem holder = html_dom_query(0, ".frvr-notifications-holder");
    if (!holder) return;
    StrBuf sb = {0};
    for (int i = 0; i < G.note_count; i++) {
        char id[16];
        snprintf(id, sizeof(id), "%d", G.notes[i].id);
        const StrikeVar v[] = {{"id", id}, {"name", G.notes[i].name[0] ? G.notes[i].name : "Player Name "}};
        append_part(&sb, "INVITE_NOTIFICATION", NULL, v, 2);
    }
    char *html = sb_finish(&sb);
    html_dom_set_html(holder, html ? html : "");
    free(html);
}

static void note_show(const Note *n) {
    Note *slot = &G.notes[G.note_count++];
    *slot = *n;
    slot->id = ++G.next_note;
    slot->timer = html_timer_add(INVITE_MS, note_expire, (void *) (intptr_t) slot->id); // notification_auto_dismiss
    notes_render();
}

// Ag(event, index): remove one, then the next waiting one shows
static void note_dismiss(const int id) {
    for (int i = 0; i < G.note_count; i++) {
        if (G.notes[i].id != id) continue;
        if (G.notes[i].timer) html_timer_cancel(G.notes[i].timer);
        memmove(&G.notes[i], &G.notes[i + 1], sizeof(G.notes[0]) * (size_t) (G.note_count - i - 1));
        G.note_count--;
        break;
    }
    if (G.note_count < NOTES_SHOWN && G.queue_count > 0) {
        const Note next = G.queue[0];
        memmove(&G.queue[0], &G.queue[1], sizeof(G.queue[0]) * (size_t) (G.queue_count - 1));
        G.queue_count--;
        note_show(&next);
        return;
    }
    notes_render();
}

static void note_expire(void *user) {
    const int id = (int) (intptr_t) user;
    for (int i = 0; i < G.note_count; i++) {
        if (G.notes[i].id == id) G.notes[i].timer = 0;
    }
    note_dismiss(id);
}

// ---------------------------------------------------------------- Hy REPORT_PLAYER

static const char *reason_error(void) {
    return G.should_validate && G.reason < 0 ? "Please choose a valid reason." : NULL;
}

static void report_info(char *out, const size_t n) {
    out[0] = 0;
    const HtmlElem t = html_dom_by_id("reasonComments");
    if (t) html_dom_get_value(t, out, n);
}

static const char *info_error(void) {
    if (!G.should_validate) return NULL;
    char text[2048];
    report_info(text, sizeof(text));
    return strlen(text) < 10 ? "More information required (10 or more characters)." : NULL;
}

// the Z7 group (dropdown + error) and RD's error / textarea class follow the view state
static void report_update(void) {
    const HtmlElem root = strike_screen_root(STRIKE_SCREEN_REPORT_PLAYER);
    if (!root) return;
    HtmlElem groups[2];
    if (html_dom_query_all(root, ".innercontent .frvr-input-group", groups, 2) < 2) return;
    const char *re = reason_error(), *ie = info_error();
    // Z7: the dropdown with "Select" + the options, is-error, the error line
    const HtmlElem old = html_dom_query(groups[0], ".dropdown-container");
    if (old) html_dom_remove(old);
    HtmlElem err;
    while ((err = html_dom_query(groups[0], ".input-error"))) html_dom_remove(err);
    char *options = NULL;
    if (G.reason_open) {
        StrBuf list = {0};
        for (int i = 0; i <= REPORT_OPTION_COUNT; i++) {
            char idx[8];
            snprintf(idx, sizeof(idx), "%d", i);
            const StrikeVar v[] = {{"class", i == 0 ? "disabled" : ""}, {"index", idx}, {"label", i == 0 ? "Select" : REPORT_OPTIONS[i - 1]}};
            append_part(&list, "REPORT_PLAYER", "option", v, 3);
        }
        char *l = sb_finish(&list);
        const StrikeVar v[] = {{"list", l ? l : ""}};
        options = part("REPORT_PLAYER", "options", v, 1);
        free(l);
    }
    const StrikeVar dv[] = {{"class", re ? "is-error" : ""}, {"label", G.reason >= 0 ? REPORT_OPTIONS[G.reason] : "Select"},
                            {"icon", G.reason_open ? "expand_less" : "expand_more"}, {"options", options ? options : ""}};
    char *dropdown = part("REPORT_PLAYER", "dropdown", dv, 4);
    html_dom_insert_html(groups[0], HTML_INSERT_BEFORE_END, dropdown);
    free(dropdown);
    free(options);
    if (re) {
        const StrikeVar v[] = {{"message", re}};
        char *e = part("REPORT_PLAYER", "error", v, 1);
        html_dom_insert_html(groups[0], HTML_INSERT_BEFORE_END, e);
        free(e);
    }
    // RD: the textarea keeps what was typed; only its class and the error line change
    while ((err = html_dom_query(groups[1], ".input-error"))) html_dom_remove(err);
    const HtmlElem t = html_dom_by_id("reasonComments");
    if (t) html_dom_set_attr(t, "class", ie ? "is-error" : "");
    if (ie) {
        const StrikeVar v[] = {{"message", ie}};
        char *e = part("REPORT_PLAYER", "error", v, 1);
        html_dom_insert_html(groups[1], HTML_INSERT_BEFORE_END, e);
        free(e);
    }
}

void strike_social_report(const int player_uid, const char *name) {
    G.report_uid = player_uid;
    snprintf(G.report_name, sizeof(G.report_name), "%s", name ? name : "");
    G.reason = -1;
    G.reason_open = G.should_validate = G.sending = 0;
    strike_screen_push(STRIKE_SCREEN_REPORT_PLAYER); // Pt(Hy, {player})
}

// kr(XA / p0, {message}): the alert replaces the report popup
static void alert(const StrikeScreen s, const char *message) {
    snprintf(G.alert, sizeof(G.alert), "%s", message);
    if (strike_screen_top() == STRIKE_SCREEN_REPORT_PLAYER) strike_screen_pop();
    strike_screen_push(s);
    KN_LOG_VERBOSE2("[strike] %s: %s\n", strike_screen_id(s), message);
}

static void report_result(const int ok) {
    G.sending = 0;
    if (ok) alert(STRIKE_SCREEN_POPUP_SUCCESS, "Player reported successfully!");
    else alert(STRIKE_SCREEN_POPUP_ERROR, "Error");
}

static void report_send(void) {
    G.should_validate = 1;
    report_update();
    if (reason_error() || info_error() || G.sending) return;
    char info[2048];
    report_info(info, sizeof(info));
    // ga().reportPlayer(alias, reason, info): kn_brain's "rpt" takes reports from accounts
    if (!active_account() || !backend_social_is_open()) {
        alert(STRIKE_SCREEN_POPUP_ERROR, "Must be logged in");
        return;
    }
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("rpt"));
    cJSON_AddItemToArray(f, cJSON_CreateString(G.report_name));
    cJSON_AddItemToArray(f, cJSON_CreateString(REPORT_OPTIONS[G.reason]));
    cJSON_AddItemToArray(f, cJSON_CreateString(info));
    backend_social_send(f);
    cJSON_Delete(f);
    G.sending = 1;
    KN_LOG_VERBOSE2("[strike] report %s: %s\n", G.report_name, REPORT_OPTIONS[G.reason]);
}

// ---------------------------------------------------------------- hooks

static int screen_in_game(const StrikeScreen s) { return s == STRIKE_SCREEN_TAP_TO_PLAY; }

static int screen_shows_friends(const StrikeScreen s) {
    return s == STRIKE_SCREEN_TAP_TO_PLAY || (s == STRIKE_SCREEN_MAIN_MENU && active_account());
}

void strike_social_render(const StrikeScreen s, const HtmlElem root) {
    if (!root) return;
    switch (s) {
    case STRIKE_SCREEN_MAIN_MENU:
        private_match_place(root, active_account() != NULL);
        friends_place(root, 0, screen_shows_friends(s));
        break;
    case STRIKE_SCREEN_TAP_TO_PLAY:
        friends_place(root, 1, 1);
        break;
    case STRIKE_SCREEN_CREATE_MATCH:
        match_render(root);
        break;
    case STRIKE_SCREEN_REPORT_PLAYER: {
        const HtmlElem name = html_dom_query(root, ".player-name strong");
        if (name) html_dom_set_text(name, G.report_name); // N.getPlayerFullName(alias, realName)
        report_update();
        break;
    }
    case STRIKE_SCREEN_POPUP_SUCCESS:
    case STRIKE_SCREEN_POPUP_ERROR: {
        const HtmlElem img = html_dom_query(root, ".alert-img");
        if (img) html_dom_set_attr(img, "src", s == STRIKE_SCREEN_POPUP_SUCCESS ? "./img/frvr-ui/info-checkmark.svg" : "./img/frvr-ui/info-close.svg");
        const HtmlElem text = html_dom_query(root, ".alert-text span");
        if (text) html_dom_set_text(text, G.alert);
        break;
    }
    default:
        break;
    }
}

static void rerender_friends(void) {
    static const StrikeScreen WITH_FRIENDS[] = {STRIKE_SCREEN_MAIN_MENU, STRIKE_SCREEN_TAP_TO_PLAY};
    for (size_t i = 0; i < sizeof(WITH_FRIENDS) / sizeof(WITH_FRIENDS[0]); i++) {
        const HtmlElem root = strike_screen_root(WITH_FRIENDS[i]);
        if (root) friends_place(root, screen_in_game(WITH_FRIENDS[i]), screen_shows_friends(WITH_FRIENDS[i]));
    }
    const HtmlElem mm = strike_screen_root(STRIKE_SCREEN_MAIN_MENU);
    if (mm) private_match_place(mm, active_account() != NULL);
    const HtmlElem cm = strike_screen_root(STRIKE_SCREEN_CREATE_MATCH);
    if (cm) match_render(cm);
}

static void open_panel(void *user) {
    (void) user;
    G.strip_hiding = 0;
    G.panel = 1; // vg = !0
    rerender_friends();
}

static void close_panel(void *user) {
    (void) user;
    G.panel_hiding = 0;
    G.panel = 0; // vg = !1
    rerender_friends();
}

int strike_social_action(const char *a) {
    if (!strcmp(a, "friends-open")) { // this.hide = !0, setTimeout(vg = !0, 50)
        if (G.panel || G.strip_hiding) return 1;
        G.strip_hiding = 1;
        rerender_friends();
        html_timer_add(50.0, open_panel, NULL);
        return 1;
    }
    if (!strcmp(a, "friends-close")) { // this.hide = !0, setTimeout(vg = !1, 300)
        if (!G.panel || G.panel_hiding) return 1;
        G.panel_hiding = 1;
        rerender_friends();
        html_timer_add(300.0, close_panel, NULL);
        return 1;
    }
    if (!strncmp(a, "friends-join:", 13)) { // quickConnect({server: friend.game})
        const Friend *f = friend_by_id(strtoll(a + 13, NULL, 10));
        if (f && f->game[0]) {
            KN_LOG_VERBOSE2("[strike] join %s's game %s\n", f->name, f->game);
            servers_ui_join_link(f->game);
        }
        return 1;
    }
    if (!strncmp(a, "friends-invite:", 15)) { // _5: en.sendGameInvite(friendId, y5(gameServer))
        Friend *f = friend_by_id(strtoll(a + 15, NULL, 10));
        char game[160];
        if (f && f->invited_until <= now_ms() && servers_ui_invite_game(game, sizeof(game))) {
            send_invite(f, game);
            f->invited_until = now_ms() + INVITE_MS;
            G.dirty = 1;
        }
        return 1;
    }
    if (!strncmp(a, "match-invite:", 13)) { // Vhe onInviteClick
        Friend *f = friend_by_id(strtoll(a + 13, NULL, 10));
        if (!f || !f->online) return 1;
        char game[160];
        if (servers_ui_invite_game(game, sizeof(game))) send_invite(f, game);
        else f->match_pending = 1;
        f->match_invited = 1;
        G.dirty = 1;
        return 1;
    }
    if (!strcmp(a, "create-match")) { // Qhe: Pt(m5)
        strike_screen_push(STRIKE_SCREEN_CREATE_MATCH);
        return 1;
    }
    if (!strcmp(a, "create-match-join")) {
        if (!servers_ui_private_match()) printf("[strike] Private Match: no director to open a private lobby\n");
        return 1;
    }
    if (!strncmp(a, "invite-dismiss:", 15)) {
        note_dismiss(atoi(a + 15));
        return 1;
    }
    if (!strncmp(a, "invite-join:", 12)) { // qhe(friendId) || b5(gameId)
        const int id = atoi(a + 12);
        for (int i = 0; i < G.note_count; i++) {
            if (G.notes[i].id != id) continue;
            char game[96];
            snprintf(game, sizeof(game), "%s", G.notes[i].game);
            note_dismiss(id);
            KN_LOG_DEBUG1("[strike] join the invite's game %s\n", game);
            servers_ui_join_link(game);
            break;
        }
        return 1;
    }
    if (!strcmp(a, "report-reason-toggle")) {
        G.reason_open = !G.reason_open;
        report_update();
        return 1;
    }
    if (!strncmp(a, "report-reason:", 14)) {
        const int i = atoi(a + 14);
        if (i <= 0 || i > REPORT_OPTION_COUNT) return 1; // "Select" is disabled
        G.reason = i - 1;
        G.reason_open = 0;
        report_update();
        return 1;
    }
    if (!strcmp(a, "report-input")) {
        if (G.should_validate) report_update();
        return 1;
    }
    if (!strcmp(a, "report-send")) {
        report_send();
        return 1;
    }
    return 0;
}

void strike_social_tick(Client *client) {
    G.client = client;
    if (!G.listening) G.listening = backend_social_listen(on_social, NULL);
    // signed in before this module listened (the login answer went by): ask now
    const int signed_in = active_account() != NULL;
    if (signed_in && !G.requested && backend_social_is_open()) {
        G.requested = 1;
        request_friends(0);
    }
    if (!signed_in && (G.requested || G.count)) {
        G.requested = 0;
        G.count = 0;
        G.dirty = 1;
    }
    if (signed_in != G.signed_in) {
        G.signed_in = signed_in;
        G.dirty = 1; // MAIN_MENU shows the friends only with an account
    }
    // invites made in Private Match before this client was in a lobby
    char game[160];
    if (servers_ui_invite_game(game, sizeof(game))) {
        for (int i = 0; i < G.count; i++) {
            if (!G.friends[i].match_pending) continue;
            G.friends[i].match_pending = 0;
            send_invite(&G.friends[i], game);
        }
    }
    // _5's 15 s: the button says Invite again
    static double next_check;
    const double now = now_ms();
    if (now >= next_check) {
        next_check = now + 500.0;
        for (int i = 0; i < G.count; i++) {
            if (G.friends[i].invited_until > 0 && G.friends[i].invited_until <= now) {
                G.friends[i].invited_until = 0;
                G.dirty = 1;
            }
        }
    }
    if (G.dirty) {
        G.dirty = 0;
        rerender_friends();
    }
}
