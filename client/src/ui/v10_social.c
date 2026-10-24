// Krunker v10.0.2's in-game Social hub on the v10 main page (doc/v10/social.md):
//   - the socialModal popup (v10 _49999, window.openSocial(tab)): sidebar tabs Social / Friends /
//     Search; the Social tab's feeds (Global / Following / Hot / Giveaways / Clan / News), New post,
//     player search; the Friends tab's friends list, presence and direct messages (the event bus);
//     the Search tab's player profile search;
//   - the view stack (_5216: .view-stack > .view-stack-overlay > .view-wrapper) with the player
//     profile (_34510, window.openPlayerProfile), the clan view (_34484, window.openClanView), the post
//     detail (_35483, window.openPost), the compose popup (_49624), the follow list (_34637) and the
//     gift / report popups;
//   - the Social menu item's unread badge and the in-game "[DM from ...]" chat lines.
// Markup is v10's (the svelte-<hash> classes of ui/css/svelte.css); data comes from kn_brain:
// gapi calls through v10_gapi() (services/brain/social_gapi.c) and the friends event bus as
// ["eb", {...}] frames on the social socket (services/brain/social_eb.c). KrunkNative adds element
// ids (v10s*) for focus / scroll restoration and tests; they carry no style.
//
// While krunker.io is the active account backend (backend_ui.h: an official krunker.io game) the hub
// shows the krunker.io account's real data, fetched as v10 fetches it: the gapi reads go to
// krunker.io's gapi with that account's token (v10_gapi.c), the friends event bus is v10's own
// socket (krunker_eb.h: <EVENT_BUS_URI>/ws?at=<fresh token>, v10 _49718, v10:139770) instead of
// kn_brain's ["eb"] frames, with v10's reconnect backoff (_49724: min(30 s, 2^n s)). Direct messages
// and read receipts are what v10 sends from the Friends tab, so they go over that socket too. What
// would change the krunker.io account or spend its KR is not ported here and reads as v10 shows it
// to a guest (actor() = NULL): Follow / Unfollow (profile, follow list, giveaway card), Like,
// comments, New post, Gift KR, Report, Trade and the clan view's Request to Join are hidden or do
// nothing; v10_gapi.c does not send their requests for that account either.
#include <backend.h>
#include <backend_ui.h>
#include <html_dom.h>
#include <html_ui.h>
#include <i18n.h>
#include <ui_logic.h>
#include <v10_social.h>
#include <v10_ui.h>
#include <follower_counts.h> // a follow moves the author's count on every copy (followers_moved)
#include <ruleset.h>
#include <weapons.h>
#include <badges.h>     // the profile's Badges tab and selected badge (badge_get / badge_texture)
#include <ranked.h>     // the profile's rank chip (ranked_rank_name / color / icon)
#include <ui_tooltip.h> // the Badges tab's tooltip
#include "../viewer/viewer_store.h" // the Creations tab's Skins: the store table and its rarities
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <v10_party.h> // v10_party_join_game: a friend's "Join" (v10_party.c)
#include <desktop_notify.h> // the same items on the desktop, when the Notifications switch is on
#include <krunker_account.h> // the krunker.io account: the event bus's fresh token, its URL
#include <krunker_eb.h>      // krunker.io's friends event bus socket
#include <client.h>          // the match's players (dm_chat_name)
#include <player_accounts.h> // a player's account (dm_chat_name)
#include <player_names.h>    // Hide player names (dm_chat_name)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


#define EB_PING_MS 25000.0
#define EB_TEXT_MAX 512
#define FEED_TABS 5
#define MAX_VIEWS 8
#define MSG_TEXT 1024

enum { TAB_SOCIAL, TAB_FRIENDS, TAB_SEARCH };
enum { EB_DISCONNECTED, EB_CONNECTING, EB_CONNECTED, EB_ERROR };
enum { MSG_PENDING, MSG_DELIVERED, MSG_FAILED };
enum { V_PROFILE, V_CLAN, V_POST, V_COMPOSE, V_FOLLOWS, V_GIFT, V_REPORT };

static const char *const k_feed_ids[FEED_TABS] = {"global", "following", "hot", "giveaways", "clan"};
static const struct {
    const char *id, *label, *icon, *empty;
} k_social_tabs[6] = {
    {"global", "Global", "public", "No posts yet."},
    {"following", "Following", "person", "You're not following anyone yet."},
    {"hot", "Hot", "whatshot", "Nothing trending right now."},
    {"giveaways", "Giveaways", "local_atm", "No active giveaways."},
    {"clan", "Clan", "groups", "Join a clan to see clan posts."},
    {"news", "News", "campaign", "No news today."},
};

typedef struct {
    int local_id;
    int64_t server_id;
    int sent;            // direction 'sent'
    char *text;
    double ts;           // ms
    int status;
    char error[96];
} Msg;

typedef struct {
    int64_t peer;
    Msg *msgs;
    int n, cap;
    int initialised, loading, has_more;
    int64_t oldest;
    int pending[32];     // FIFO of local ids waiting for msg:sent
    int npending;
} Conv;

typedef struct {
    int loading, loaded;
    char error[128];
    cJSON *posts;        // owned array
} Feed;

typedef struct {
    int type;
    char name[64];       // player / clan name
    int64_t id;          // post id / clan id
    int loading;
    char error[160];
    cJSON *data;         // owned: profile / clan / {post, comments} / follow rows
    // profile
    int tab;             // profile right-side tab
    int ctab;            // the Creations tab's sub-tab: 0 Skins (v10's default), 1 Games, 2 Mods, 3 Assets
    int *cskins, ncskins, cskins_ready; // the skins this player created (store indices), found once
    int follow_busy;
    int sales_range;
    cJSON *sales, *listings, *feed, *reports; // owned
    int sales_loading, listings_loading, feed_loading, reports_loading;
    // clan view
    int applied, apply_busy;
    char apply_error[128];
    // post detail
    int sending;
    char reply_error[128];
    // compose
    int giveaway, submitting;
    char compose_error[160];
    int depth;           // this view's position on the shared v10 view stack (v10_ui.h)
    // follow list
    int follows_tab;     // 0 followers, 1 following
    cJSON *followers, *following;
    // gift / report
    int done;
    char message[160];
} View;

static struct {
    int inited, open, tab, rendered;
    int sub;                         // social sub-tab index (k_social_tabs)
    Feed feeds[FEED_TABS];
    Feed news;
    char social_query[96];
    // search tab
    char search_q[96];
    int search_seq, search_loading, searched, search_timer;
    char search_error[128];
    cJSON *results;
    // event bus
    int eb_status;
    char eb_error[160];
    cJSON *friends;                  // owned array of friend objects
    int friends_more, friends_loading, friends_next;
    Conv *convs;
    int nconv, cap_conv;
    int next_local;
    int64_t selected;                // selected friend id (0 none)
    char friend_filter[64];
    char compose[EB_TEXT_MAX * 4 + 8];
    int64_t account_id;              // the account the bus is for (krunker.io: bus_account()'s key)
    int backend;                     // the account backend the hub last showed (AccountBackend)
    // krunker.io's event bus (krunker_eb.h)
    unsigned kio_gen;                // bumped per (re)connect / reset: a late token is dropped
    int kio_backoff;                 // v10 _49698: reconnect attempts since the last open
    double kio_retry_at;             // ms of the next reconnect (0 none)
    double last_ping;
    int list_requested;
    // view stack
    View views[MAX_VIEWS];
    int nviews;
    int unread_shown;                // the menu badge's last value
    int scenario;
    // the conversation's load-earlier-on-scroll (v10.0.3 deob-10.0.3.js:157767-157790)
    float earlier_anchor;            // scrollHeight - scrollTop when an earlier page was asked (0 none): kept after it lands
    float msgs_top;                  // #v10sMessages' scrollTop at the last tick (-1 unknown)
    int64_t msgs_peer;               // the conversation msgs_top belongs to
} S;

// ------------------------------------------------------------------ small helpers

static const char *jstr(const cJSON *o, const char *k) {
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(o, k));
    return s ? s : "";
}

static double jnum(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItem(o, k);
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    if (cJSON_IsString(v)) return atof(v->valuestring);
    return 0;
}

static int jtrue(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return v && !cJSON_IsFalse(v) && !cJSON_IsNull(v) && !(cJSON_IsNumber(v) && v->valuedouble == 0) &&
           !(cJSON_IsString(v) && !v->valuestring[0]);
}

static const char *arg_str(const HtmlArg *args, const int argc, const int i) {
    if (i >= argc) return "";
    if (args[i].kind == HTML_ARG_STR && args[i].str) return args[i].str;
    return "";
}

static double arg_num(const HtmlArg *args, const int argc, const int i, const double def) {
    if (i >= argc) return def;
    if (args[i].kind == HTML_ARG_NUM || args[i].kind == HTML_ARG_BOOL) return args[i].num;
    if (args[i].kind == HTML_ARG_STR && args[i].str) return atof(args[i].str);
    return def;
}

// v10 _8983: x.toString().replace(/\B(?=(\d{3})+(?!\d))/g, ",")
static void commas(char *out, const size_t n, const double v) {
    char digits[48];
    snprintf(digits, sizeof(digits), "%.0f", v);
    const char *d = digits;
    size_t o = 0;
    if (*d == '-' && o + 1 < n) out[o++] = *d++;
    const size_t len = strlen(d);
    for (size_t i = 0; i < len && o + 2 < n; i++) {
        if (i && (len - i) % 3 == 0) out[o++] = ',';
        out[o++] = d[i];
    }
    out[o] = 0;
}

// v10 _8991 (one argument): "1y 2d 3h 4m " (minutes always, never seconds, trailing space)
static void fmt_dur(char *out, const size_t n, const double ms) {
    long long s = ms > 0 ? (long long) floor(ms / 1000.0) : 0;
    long long m = s / 60, h = m / 60;
    m %= 60;
    long long d = h / 24;
    h %= 24;
    const long long y = d / 365;
    d %= 365;
    size_t o = 0;
    out[0] = 0;
    if (y) o += (size_t) snprintf(out + o, n - o, "%lldy ", y);
    if (d && o < n) o += (size_t) snprintf(out + o, n - o, "%lldd ", d);
    if (h && o < n) o += (size_t) snprintf(out + o, n - o, "%lldh ", h);
    if (o < n) snprintf(out + o, n - o, "%lldm ", m);
}

static const char *const k_months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char *const k_days[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double) ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void local_tm(const double ms, struct tm *out) {
    const time_t t = (time_t) (ms / 1000.0);
#ifdef _WIN32 // MinGW's msvcrt declares neither gmtime_r nor localtime_r (v10_market.c's pattern)
    if (S.scenario) gmtime_s(out, &t); // --render-scenario: the same frame in every time zone
    else localtime_s(out, &t);
#else
    if (S.scenario) gmtime_r(&t, out); // --render-scenario: the same frame in every time zone
    else localtime_r(&t, out);
#endif
}

// toLocaleTimeString([], {hour: '2-digit', minute: '2-digit'}) in en-US: "03:07 PM"
static void fmt_clock(char *out, const size_t n, const double ms) {
    struct tm tm;
    local_tm(ms, &tm);
    const int h12 = tm.tm_hour % 12 ? tm.tm_hour % 12 : 12;
    snprintf(out, n, "%02d:%02d %s", h12, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
}

// new Date(ts).toLocaleString() in en-US: "10/13/2026, 3:07:12 PM"
static void fmt_full(char *out, const size_t n, const double ms) {
    struct tm tm;
    local_tm(ms, &tm);
    const int h12 = tm.tm_hour % 12 ? tm.tm_hour % 12 : 12;
    snprintf(out, n, "%d/%d/%d, %d:%02d:%02d %s", tm.tm_mon + 1, tm.tm_mday, tm.tm_year + 1900, h12, tm.tm_min, tm.tm_sec,
             tm.tm_hour < 12 ? "AM" : "PM");
}

// the conversation's day divider: Today / Yesterday / "Monday, Oct 12" (+ ", 2025" another year)
static void fmt_day(char *out, const size_t n, const double ms) {
    struct tm tm, today, yday;
    local_tm(ms, &tm);
    const double now = S.scenario ? ms : now_ms();
    local_tm(now, &today);
    local_tm(now - 86400000.0, &yday);
    if (tm.tm_year == today.tm_year && tm.tm_yday == today.tm_yday) snprintf(out, n, "Today");
    else if (tm.tm_year == yday.tm_year && tm.tm_yday == yday.tm_yday) snprintf(out, n, "Yesterday");
    else if (tm.tm_year == today.tm_year) snprintf(out, n, "%s, %s %d", k_days[tm.tm_wday], k_months[tm.tm_mon], tm.tm_mday);
    else snprintf(out, n, "%s, %s %d, %d", k_days[tm.tm_wday], k_months[tm.tm_mon], tm.tm_mday, tm.tm_year + 1900);
}

// v10 profile fmtDate: "YYYY-MM-DD..." -> "4 Oct 2021"; null -> "None"
static void fmt_date(char *out, const size_t n, const char *iso) {
    int y = 0, mo = 0, d = 0;
    if (!iso || sscanf(iso, "%d-%d-%d", &y, &mo, &d) != 3 || mo < 1 || mo > 12) {
        snprintf(out, n, "None");
        return;
    }
    snprintf(out, n, "%d %s %d", d, k_months[mo - 1], y);
}

// v10 clan member score: abbreviated (1234 -> 1.2k)
static void fmt_abbr(char *out, const size_t n, const double v) {
    if (v < 1000) {
        snprintf(out, n, "%.0f", v);
        return;
    }
    static const char *const suf[] = {"", "k", "m", "b", "t"};
    char digits[48];
    snprintf(digits, sizeof(digits), "%.0f", v);
    int e = (int) strlen(digits) / 3;
    if (e > 4) e = 4;
    double x = v / pow(1000.0, e);
    for (int prec = 2; prec >= 1; prec--) {
        char t[32];
        snprintf(t, sizeof(t), "%.*g", prec, x);
        x = atof(t);
        int dig = 0;
        for (const char *c = t; *c; c++) dig += *c >= '0' && *c <= '9';
        if (dig <= 2) break;
    }
    if (x == floor(x)) snprintf(out, n, "%.0f%s", x, suf[e]);
    else snprintf(out, n, "%.1f%s", x, suf[e]);
}

static int level_of(const double score) {
    const int l = (int) floor(0.03 * sqrt(score > 0 ? score : 0));
    return l < 1 ? 1 : l;
}

static int my_level(void) {
    const AccountInfo *a = active_account();
    return a ? a->level : 0;
}

static int64_t my_id(void) {
    const AccountInfo *a = active_account();
    return a ? a->id : 0;
}

// krunker.io is the active account backend: the hub reads the krunker.io account's data
static int kio(void) { return account_backend() == ACCOUNT_BACKEND_KRUNKER; }

// The account the hub acts as: none for the krunker.io account (its writes are not ported: the
// views show what v10 shows a guest for them), else the active account.
static const AccountInfo *actor(void) { return kio() ? NULL : active_account(); }

// The account the friends bus is for: kn_brain's id, or (krunker.io, whose account has no kn_brain
// id) a negative key of the signed-in krunker.io account's name; 0 = none (guest).
static int64_t bus_account(void) {
    if (!kio()) return my_id();
    if (!krunker_account_signed_in()) return 0;
    uint32_t h = 2166136261u;
    for (const char *c = krunker_account_name(); *c; c++) h = (h ^ (unsigned char) *c) * 16777619u;
    return -1 - (int64_t) (h & 0x7fffffff);
}

// v10 _34422: staff clan tags are gold
static const char *clan_color(const char *tag, const char *fallback) {
    static const char *const gold[] = {"arae", "lain", "scte", "fame", "kpd", "jump", "art", "dev", "mmok", "nl"};
    char low[16];
    size_t i = 0;
    for (; tag && tag[i] && i + 1 < sizeof(low); i++) low[i] = (char) (tag[i] >= 'A' && tag[i] <= 'Z' ? tag[i] + 32 : tag[i]);
    low[i] = 0;
    for (size_t k = 0; k < sizeof(gold) / sizeof(gold[0]); k++)
        if (!strcmp(low, gold[k])) return "#FBC02D";
    return fallback;
}

// a JS string argument inside an inline handler attribute (double-quoted attribute, single-quoted arg)
static void sb_arg(StrBuf *sb, const char *s) {
    sb_append(sb, "'");
    for (; s && *s; s++) {
        if (*s == '\'' || *s == '\\') sb_append(sb, "\\");
        if (*s == '"') sb_append(sb, "&quot;");
        else if (*s == '&') sb_append(sb, "&amp;");
        else if (*s == '<') sb_append(sb, "&lt;");
        else sb_append_n(sb, s, 1);
    }
    sb_append(sb, "'");
}

static void render(void);
static void render_views(void);
static void dump_state(void);
static void views_clear(void);

// the v10 page is up (its Svelte menu's #menuBtnSideCommunity exists only there)
static int ui_page_is_v10(void) { return html_dom_by_id("menuBtnSideCommunity") != 0; }

// ------------------------------------------------------------------ friends event bus (store _49849)

static cJSON *friend_by_id(const int64_t id) {
    cJSON *f;
    cJSON_ArrayForEach(f, S.friends) if ((int64_t) jnum(f, "id") == id) return f;
    return NULL;
}

static const char *friend_name(const int64_t id) {
    const cJSON *f = friend_by_id(id);
    return f ? jstr(f, "name") : "";
}

// A friend's name in the in-game chat (Hide player names): a friend playing in this match reads as its player does
// (Player_<n> by its uid, never the text), anyone else keeps the friend's name.
static const char *dm_chat_name(const int64_t account, const char *name, char *out, const size_t size) {
    const Client *client = menu_ui() ? menu_ui()->client : NULL;
    if (!player_names_hidden() || !client) return name;
    for (size_t i = 0; i < client->game.player_count; i++) {
        const Player *p = client->game.players[i];
        if (!p) continue;
        const NetPlayerInfoPacket *info = player_accounts_get(p->uid);
        if (!info || info->account_id != account) continue;
        const char *shown = player_names_shown(p->uid, p == client->me, p->name[0] ? p->name : name, out, size);
        return shown ? shown : name;
    }
    return name;
}

static Conv *conv_get(const int64_t peer, const int create) {
    for (int i = 0; i < S.nconv; i++)
        if (S.convs[i].peer == peer) return &S.convs[i];
    if (!create) return NULL;
    if (S.nconv == S.cap_conv) {
        const int cap = S.cap_conv ? S.cap_conv * 2 : 16;
        Conv *n = realloc(S.convs, (size_t) cap * sizeof(*n));
        if (!n) return NULL;
        S.convs = n;
        S.cap_conv = cap;
    }
    Conv *c = &S.convs[S.nconv++];
    memset(c, 0, sizeof(*c));
    c->peer = peer;
    return c;
}

static Msg *conv_insert(Conv *c, const int at) {
    if (c->n == c->cap) {
        const int cap = c->cap ? c->cap * 2 : 32;
        Msg *n = realloc(c->msgs, (size_t) cap * sizeof(*n));
        if (!n) return NULL;
        c->msgs = n;
        c->cap = cap;
    }
    memmove(&c->msgs[at + 1], &c->msgs[at], (size_t) (c->n - at) * sizeof(Msg));
    c->n++;
    Msg *m = &c->msgs[at];
    memset(m, 0, sizeof(*m));
    m->local_id = ++S.next_local;
    return m;
}

static int conv_has_server(const Conv *c, const int64_t id) {
    for (int i = 0; i < c->n; i++)
        if (c->msgs[i].server_id == id) return 1;
    return 0;
}

static void convs_clear(void) {
    for (int i = 0; i < S.nconv; i++) {
        for (int k = 0; k < S.convs[i].n; k++) free(S.convs[i].msgs[k].text);
        free(S.convs[i].msgs);
    }
    free(S.convs);
    S.convs = NULL;
    S.nconv = S.cap_conv = 0;
}

static int unread_of(const int64_t id) {
    const cJSON *f = friend_by_id(id);
    return f ? (int) jnum(f, "unread") : 0;
}

static void set_unread(const int64_t id, const int n) {
    cJSON *f = friend_by_id(id);
    if (!f) return;
    cJSON_DeleteItemFromObject(f, "unread");
    cJSON_AddNumberToObject(f, "unread", n);
}

// unread counts of peers that are not (yet) in the friends list (unread:summary before friends:list)
static struct {
    int64_t id;
    int n;
    char name[32];
} g_unread_extra[32];

int v10_social_unread_total(void) {
    int total = 0;
    const cJSON *f;
    cJSON_ArrayForEach(f, S.friends) total += (int) jnum(f, "unread");
    for (size_t i = 0; i < sizeof(g_unread_extra) / sizeof(g_unread_extra[0]); i++)
        if (g_unread_extra[i].id && !friend_by_id(g_unread_extra[i].id)) total += g_unread_extra[i].n;
    return total;
}

const cJSON *v10_social_friends(void) { return S.friends; }

// the profile window's Friends tab (windows[4] tab 1): v10's #friendList rows, Join for a friend in
// a game (openURL('/?game=<id>', 1)), Remove Friend (deleteFollower: unfollow)
// The friends list that never came (the friends bus is down, or it never connected): after the deadline the tab
// says so instead of "Loading Friends..." for good. The list coming later clears it.
#define FRIENDS_WAIT_MS 15000
static int s_friends_timer, s_friends_failed;
static void friends_expired(void *user) {
    (void) user;
    s_friends_timer = 0;
    if (S.friends) return;
    s_friends_failed = 1;
    menu_rerender_if_open(5); // window 5's Friends tab
}

char *v10_social_window_friends_html(void) {
    if (!S.friends) {
        if (s_friends_failed) {
            StrBuf sb = {0};
            sb_append(&sb, "<div id='friendList'><span style='color:#9a9a9a'>");
            sb_append_esc(&sb, I18N("kn.friends.loadFailed", "Couldn't load your friends right now. Reopen this tab to try again."));
            sb_append(&sb, "</span></div>");
            return sb_finish(&sb);
        }
        if (!s_friends_timer) s_friends_timer = html_timer_add(FRIENDS_WAIT_MS, friends_expired, NULL);
        return NULL;
    }
    html_timer_cancel(s_friends_timer);
    s_friends_timer = 0;
    s_friends_failed = 0;
    StrBuf sb = {0};
    sb_append(&sb, "<div id='friendList'>");
    if (cJSON_GetArraySize(S.friends) == 0) sb_append(&sb, "<span style='color:#9a9a9a'>You don't have any friends yet.</span>");
    // online friends first, the list's order otherwise (v10 sorts by st, stably, deob-10.0.3.js:255470-255481). v10
    // also writes a friend's status message (sm) for "Online" and a featured check_circle (plf): the bus's friend
    // objects carry neither
    int i = 0;
    for (int pass = 0; pass < 2; pass++) {
        const cJSON *f;
        cJSON_ArrayForEach(f, S.friends) {
            const char *name = jstr(f, "name"), *game = jstr(f, "gameInstanceId");
            const int online = jtrue(f, "online");
            if (online != !pass) continue;
            sb_append(&sb, "<div><div style='margin-bottom:10px;text-align:left;padding-bottom:10px;border-bottom:3px solid #ffffff33;padding-top:10px;'>"
                           "<img class='folPfp' onerror='this.onerror=null;this.src=\"./img/player_pfp.png\";' src='./img/player_pfp.png' /><div class='folInfHldr'>");
            if (jtrue(f, "premium")) sb_append(&sb, "<i class='material-icons' style='color:#FBC02D;margin-top:-8px;font-size:28px;vertical-align:middle;'>beenhere</i>");
            sb_append(&sb, "<a href='javascript:void(0)' onclick=\"openPlayerProfile(");
            sb_arg(&sb, name);
            sb_append(&sb, ")\" class='lName' style='color:rgba(255,255,255,0.9)'>");
            sb_append_esc(&sb, name);
            sb_appendf(&sb, "</a><div style='color:rgba(255,255,255,0.4);font-size:16px;margin-top:4px;max-width:500px;white-space:nowrap;text-overflow:ellipsis;overflow:hidden;'>%s</div></div><div style='float:right;'>",
                       online ? "<div class='onStatus' style='background:#27ab32'></div>Online" : "<div class='onStatus' style='background:#7f7f7f'></div>Offline");
            if (game[0]) {
                char url[160];
                snprintf(url, sizeof(url), "/?game=%s", game);
                sb_append(&sb, "<div class='button buttonG' style='padding:10px;margin-right:15px;' onclick=\"openURL(");
                sb_arg(&sb, url);
                sb_append(&sb, ",1)\">Join</div>");
            }
            if (!kio()) { // krunker.io account: unfollowing is not ported (it changes that account)
                sb_append(&sb, "<div class='button buttonR' style='padding:10px;' onclick=\"deleteFollower(");
                sb_arg(&sb, name);
                sb_appendf(&sb, ",%d)\">Remove Friend</div>", i);
            }
            i++;
            sb_append(&sb, "</div></div></div>");
        }
    }
    sb_append(&sb, "</div>");
    return sb_finish(&sb);
}

static void eb_send(cJSON *obj) {
    if (kio()) { // v10 _49728: JSON text on the event bus socket (dropped when it is not open)
        char *t = cJSON_PrintUnformatted(obj);
        if (t) krunker_eb_send(t);
        free(t);
        cJSON_Delete(obj);
        return;
    }
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("eb"));
    cJSON_AddItemToArray(f, obj);
    backend_social_send(f);
    cJSON_Delete(f);
}

static void eb_send_type(const char *type, const char *k1, const double v1) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", type);
    if (k1) cJSON_AddNumberToObject(o, k1, v1);
    eb_send(o);
}

static int bus_open(void) { return kio() ? krunker_eb_is_open() : backend_social_is_open(); }

static void eb_reset(void) {
    krunker_eb_close(); // (krunker.io's socket: v10 closes it with 1000 on logout)
    S.kio_gen++;
    S.kio_backoff = 0;
    S.kio_retry_at = 0;
    cJSON_Delete(S.friends);
    S.friends = NULL;
    S.friends_more = S.friends_loading = S.friends_next = 0;
    convs_clear();
    memset(g_unread_extra, 0, sizeof(g_unread_extra));
    S.selected = 0;
    S.eb_status = EB_DISCONNECTED;
    S.eb_error[0] = 0;
    S.list_requested = 0;
}

// onopen: connected, the first friends page, the ping
static void eb_connected(void) {
    S.eb_status = EB_CONNECTED;
    S.eb_error[0] = 0;
    S.friends_loading = 1;
    S.list_requested = 1;
    eb_send_type("friends:list", "offset", 0);
    S.last_ping = now_ms();
}

static void eb_load_more(void) {
    if (S.friends_loading || !S.friends_more) return;
    S.friends_loading = 1;
    eb_send_type("friends:list", "offset", S.friends_next);
}

static void eb_on_friends(const cJSON *o) {
    S.friends_loading = 0;
    const cJSON *list = cJSON_GetObjectItem(o, "friends");
    const int offset = (int) jnum(o, "offset");
    if (offset == 0 || !S.friends) {
        cJSON_Delete(S.friends);
        S.friends = cJSON_CreateArray();
    }
    const cJSON *f;
    int count = 0;
    cJSON_ArrayForEach(f, list) {
        cJSON *copy = cJSON_Duplicate(f, 1);
        const int64_t id = (int64_t) jnum(f, "id");
        // an unread:summary that came first keeps its count
        for (size_t i = 0; i < sizeof(g_unread_extra) / sizeof(g_unread_extra[0]); i++) {
            if (g_unread_extra[i].id == id && g_unread_extra[i].n > (int) jnum(f, "unread")) {
                cJSON_DeleteItemFromObject(copy, "unread");
                cJSON_AddNumberToObject(copy, "unread", g_unread_extra[i].n);
            }
            if (g_unread_extra[i].id == id) g_unread_extra[i].id = 0;
        }
        cJSON_AddItemToArray(S.friends, copy);
        count++;
        // seed the conversation with the last message
        if (jnum(f, "lastId") > 0 && jstr(f, "lastText")[0]) {
            Conv *c = conv_get(id, 1);
            if (c && c->n == 0) {
                Msg *m = conv_insert(c, 0);
                if (m) {
                    m->server_id = (int64_t) jnum(f, "lastId");
                    m->sent = jtrue(f, "lastFromMe");
                    m->text = strdup(jstr(f, "lastText"));
                    m->ts = jnum(f, "lastTs");
                    m->status = MSG_DELIVERED;
                }
            }
        }
    }
    S.friends_more = jtrue(o, "hasMore");
    S.friends_next = offset + count;
}

// ------------------------------------------------------------------ notifications
// The notification center (v10_notify.c) carries v10's set only (Wild Offer, Survey, Register): the
// event bus's news is not in it. A direct message shows in the game chat and the Friends tab's unread
// badge; a follow not followed back is a Followers entry in the Inbox (windows[29], the mail count).
// KrunkNative's desktop notifier (desktop_notify.c) still posts a follow and a message that is not on
// screen to the OS.

// A follow (kn_brain's friend:request: a follow not followed back) and its acceptance: the OS
// notification only; the menu shows them as the Inbox's Followers entry.
static void notify_friend(const cJSON *o, const int accepted) {
    const char *name = jstr(o, "name");
    if (!name[0]) return;
    char text[192];
    if (accepted) I18N_F(text, "kn.notify.friendAccepted", "{0} followed you back. You are now friends.", name);
    else I18N_F(text, "kn.notify.friendRequest", "{0} followed you. Follow them back to become friends.", name);
    const char *title = accepted ? I18N("kn.notify.friendAcceptedTitle", "Friend Added") : I18N("kn.notify.friendRequestTitle", "Friend Request");
    desktop_notify_item(title, text);
}

static void dm_chat_line(const char *text) {
    // v10 _69625(-1, null, text, 2, "#5ce05a"): an in-game system chat line
    hud_ui_add_chat(NULL, text, "#5ce05a");
}

static void eb_on_recv(const cJSON *o) {
    const int64_t from = (int64_t) jnum(o, "from"), id = (int64_t) jnum(o, "id");
    Conv *c = conv_get(from, 1);
    if (!c || (id && conv_has_server(c, id))) return;
    Msg *m = conv_insert(c, c->n);
    if (!m) return;
    m->server_id = id;
    m->text = strdup(jstr(o, "text"));
    m->ts = jnum(o, "ts");
    m->status = MSG_DELIVERED;
    set_unread(from, unread_of(from) + 1);
    char line[MSG_TEXT + 96];
    const char *name = friend_name(from);
    char shown[PLAYER_NAME_SIZE];
    snprintf(line, sizeof(line), "[DM from %s]: %s", dm_chat_name(from, name[0] ? name : "Unknown", shown, sizeof(shown)), jstr(o, "text"));
    dm_chat_line(line); // v10 _49706 listeners: every new message, on any page (the chat list keeps it for the game)
    if (!(S.open && S.tab == TAB_FRIENDS && S.selected == from)) { // KrunkNative: not on screen: the OS notification
        char title[96];
        I18N_F(title, "kn.notify.dmTitle", "Message from {0}", name[0] ? name : "Unknown");
        desktop_notify_item(title, jstr(o, "text"));
    }
}

static void eb_on_sent(const cJSON *o) {
    Conv *c = conv_get((int64_t) jnum(o, "to"), 0);
    if (!c || c->npending == 0) return;
    const int local = c->pending[0];
    memmove(c->pending, c->pending + 1, (size_t) (--c->npending) * sizeof(int));
    for (int i = 0; i < c->n; i++) {
        if (c->msgs[i].local_id != local) continue;
        c->msgs[i].status = MSG_DELIVERED;
        c->msgs[i].ts = jnum(o, "ts");
        c->msgs[i].server_id = (int64_t) jnum(o, "id");
        break;
    }
}

static void eb_on_unread_summary(const cJSON *o) {
    cJSON *f;
    cJSON_ArrayForEach(f, S.friends) {
        cJSON_DeleteItemFromObject(f, "unread");
        cJSON_AddNumberToObject(f, "unread", 0);
    }
    memset(g_unread_extra, 0, sizeof(g_unread_extra));
    const cJSON *e;
    size_t slot = 0;
    cJSON_ArrayForEach(e, cJSON_GetObjectItem(o, "counts")) {
        const int64_t id = e->string ? strtoll(e->string, NULL, 10) : 0;
        const int n = (int) jnum(e, "count");
        if (id <= 0 || n <= 0) continue;
        if (friend_by_id(id)) set_unread(id, n);
        else if (slot < sizeof(g_unread_extra) / sizeof(g_unread_extra[0])) {
            g_unread_extra[slot].id = id;
            g_unread_extra[slot].n = n;
            snprintf(g_unread_extra[slot].name, sizeof(g_unread_extra[slot].name), "%s", jstr(e, "name"));
            slot++;
        }
        char line[160];
        const char *name = jstr(e, "name");
        char shown[PLAYER_NAME_SIZE];
        const char *who = dm_chat_name(id, name[0] ? name : "a friend", shown, sizeof(shown));
        snprintf(line, sizeof(line), "[DM] %d new %s from %s", n, n == 1 ? "message" : "messages", who);
        dm_chat_line(line); // v10 _49707 listeners: each friend with unread messages at sign-in
    }
}

static void eb_on_history(const cJSON *o) {
    const int64_t peer = (int64_t) jnum(o, "peer");
    Conv *c = conv_get(peer, 1);
    if (!c) return;
    const int64_t me = my_id();
    int at = 0;
    const cJSON *m;
    cJSON_ArrayForEach(m, cJSON_GetObjectItem(o, "messages")) {
        const int64_t id = (int64_t) jnum(m, "id");
        if (conv_has_server(c, id)) continue;
        Msg *x = conv_insert(c, at++);
        if (!x) break;
        x->server_id = id;
        // krunker.io: the account's id is not known here; a 1:1 conversation's message not from the
        // peer is ours (v10: from === account.id)
        x->sent = kio() ? (int64_t) jnum(m, "from") != peer : (int64_t) jnum(m, "from") == me;
        x->text = strdup(jstr(m, "text"));
        x->ts = jnum(m, "ts");
        x->status = MSG_DELIVERED;
    }
    c->initialised = 1;
    c->loading = 0;
    c->has_more = jtrue(o, "hasMore");
    if (c->n > 0 && c->msgs[0].server_id) c->oldest = c->msgs[0].server_id;
}

static void eb_on_presence(const cJSON *o, const int online) {
    cJSON *f = friend_by_id((int64_t) jnum(o, "id"));
    if (!f) return;
    cJSON_DeleteItemFromObject(f, "online");
    cJSON_AddBoolToObject(f, "online", online);
    if (!online) {
        cJSON_DeleteItemFromObject(f, "gameInstanceId");
        cJSON_DeleteItemFromObject(f, "gamePasswordProtected");
    }
}

static void eb_on_game(const cJSON *o) {
    cJSON *f = friend_by_id((int64_t) jnum(o, "id"));
    if (!f) return;
    cJSON_DeleteItemFromObject(f, "gameInstanceId");
    cJSON_DeleteItemFromObject(f, "gamePasswordProtected");
    const char *g = jstr(o, "gameInstanceId");
    if (g[0]) {
        cJSON_AddStringToObject(f, "gameInstanceId", g);
        cJSON_AddBoolToObject(f, "gamePasswordProtected", jtrue(o, "gamePasswordProtected"));
    }
}

static void eb_on_error(const cJSON *o) {
    const char *code = jstr(o, "code");
    const char *text = v10_gapi_error_text(o, I18N("errors.common.unknown", "An unexpected error occurred. Please try again."));
    // a refused frame (social_ws.c's rate limit) names the frame it refuses in "of": only a refused
    // msg:send is a failed message; the others release their own loading flag and show the brain's text
    const char *of = jstr(o, "of");
    // an error of another request (an unknown type, or game:invite: strike_social.c's own; the brain names it in `of`)
    // is shown, and never fails a pending msg:send bubble
    if (of[0] && strcmp(of, "msg:send") && strcmp(code, "EB_RATE_LIMITED")) {
        snprintf(S.eb_error, sizeof(S.eb_error), "%s", text);
        return;
    }
    if (!strcmp(code, "EB_RATE_LIMITED") && of[0] && strcmp(of, "msg:send")) {
        if (!strcmp(of, "friends:list")) S.friends_loading = 0;
        if (!strcmp(of, "msg:history"))
            for (int i = 0; i < S.nconv; i++) S.convs[i].loading = 0;
        snprintf(S.eb_error, sizeof(S.eb_error), "%s", jstr(o, "message"));
        return;
    }
    if (strcmp(code, "EB_TARGET_OFFLINE") && strcmp(code, "EB_NOT_FRIEND") && strcmp(code, "EB_RATE_LIMITED") &&
        strcmp(code, "EB_TEXT_TOO_LONG") && strcmp(code, "EB_MSG_INVALID")) {
        snprintf(S.eb_error, sizeof(S.eb_error), "%s", text);
        return;
    }
    // the oldest pending message (smallest local id, any peer) failed
    Conv *best = NULL;
    for (int i = 0; i < S.nconv; i++)
        if (S.convs[i].npending && (!best || S.convs[i].pending[0] < best->pending[0])) best = &S.convs[i];
    if (!best) return;
    const int local = best->pending[0];
    memmove(best->pending, best->pending + 1, (size_t) (--best->npending) * sizeof(int));
    for (int i = 0; i < best->n; i++) {
        if (best->msgs[i].local_id != local) continue;
        best->msgs[i].status = MSG_FAILED;
        snprintf(best->msgs[i].error, sizeof(best->msgs[i].error), "%s", text);
    }
}

static void eb_dispatch(const cJSON *o);

static void on_social(void *user, const cJSON *frame) {
    (void) user;
    const char *label = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 0));
    if (!label || kio()) return; // krunker.io: the bus is krunker.io's socket (the kn_brain one is held)
    if (!strcmp(label, "__close")) {
        if (S.account_id) {
            S.eb_status = EB_ERROR;
            if (!S.eb_error[0]) snprintf(S.eb_error, sizeof(S.eb_error), "Event-bus connection lost.");
            S.list_requested = 0;
            if (S.open) render();
        }
        return;
    }
    if (!strcmp(label, "a")) { // a login answer: the bus is (re)connected for this account
        const AccountInfo *a = active_account();
        if (cJSON_IsNull(cJSON_GetArrayItem(frame, 1)) && a) {
            S.account_id = a->id;
            eb_connected();
            if (S.open) render();
        }
        return;
    }
    if (strcmp(label, "eb") != 0) return;
    eb_dispatch(cJSON_GetArrayItem(frame, 1));
}

// one event bus message (kn_brain's ["eb", o] or krunker.io's socket), v10's onmessage switch
static void eb_dispatch(const cJSON *o) {
    const char *type = jstr(o, "type");
    if (!strcmp(type, "friends:list")) eb_on_friends(o);
    else if (!strcmp(type, "msg:recv")) eb_on_recv(o);
    else if (!strcmp(type, "msg:sent")) eb_on_sent(o);
    else if (!strcmp(type, "unread:summary")) eb_on_unread_summary(o);
    else if (!strcmp(type, "msg:history:result")) eb_on_history(o);
    else if (!strcmp(type, "presence:online")) eb_on_presence(o, 1);
    else if (!strcmp(type, "presence:offline")) eb_on_presence(o, 0);
    else if (!strcmp(type, "presence:game")) eb_on_game(o);
    else if (!strcmp(type, "error")) eb_on_error(o);
    else if (!strcmp(type, "friend:request")) notify_friend(o, 0); // KrunkNative (kn_brain social_eb.c): the OS notification; the menu's is the Inbox's Followers entry
    else if (!strcmp(type, "friend:accepted")) notify_friend(o, 1);
    else return; // presence:pong, ranked:queued (no notice on the menu)
    if (!strcmp(type, "friends:list") || !strncmp(type, "presence:", 9)) menu_rerender_if_open(5); // window 5's Friends tab
    // an open conversation reads what arrives (v10: msg:read whenever its length changes)
    if (S.open && S.tab == TAB_FRIENDS && S.selected && !strcmp(type, "msg:recv") &&
        (int64_t) jnum(o, "from") == S.selected) {
        set_unread(S.selected, 0);
        eb_send_type("msg:read", "peer", (double) S.selected);
    }
    if (S.open) render();
}

static int g_generation; // (defined with the gapi helpers below)

// ---- krunker.io's event bus socket (v10 _49718 connect, _49724 reconnect)

static void kio_retry(void) {
    if (S.kio_retry_at) return;
    double ms = 1000.0 * pow(2.0, S.kio_backoff);
    if (ms > 30000.0) ms = 30000.0;
    S.kio_backoff++;
    S.kio_retry_at = now_ms() + ms;
}

static void kio_fail(const char *text) {
    S.eb_status = EB_ERROR;
    if (text) snprintf(S.eb_error, sizeof(S.eb_error), "%s", text); // NULL: an error already said keeps
    S.list_requested = 0;
    kio_retry();
    if (S.open) render();
}

static void kio_token(void *user, const char *token) {
    if ((unsigned) (uintptr_t) user != S.kio_gen || !kio() || !S.account_id) return;
    if (!token || !token[0]) {
        kio_fail("Not signed in.");
        return;
    }
    char enc[6000], path[6100], url[6400];
    backend_url_encode(enc, sizeof(enc), token);
    snprintf(path, sizeof(path), "ws?at=%s", enc);
    krunker_service_url(KRUNKER_SVC_EVENT_BUS, path, url, sizeof(url));
    const int opened = krunker_eb_open(url);
    memset(url, 0, sizeof(url));
    memset(path, 0, sizeof(path));
    memset(enc, 0, sizeof(enc));
    if (!opened) kio_fail("Could not open the event-bus connection.");
}

static void kio_connect(void) {
    S.kio_retry_at = 0;
    S.eb_status = EB_CONNECTING;
    S.eb_error[0] = 0;
    S.list_requested = 1;
    krunker_account_fresh_token(kio_token, (void *) (uintptr_t) ++S.kio_gen);
}

static void kio_event(void *user, const int event, const char *text, const int code) {
    (void) user;
    if (!kio() || !S.account_id) return;
    if (event == KRUNKER_EB_OPEN) { // onopen: connected, friends:list {offset: 0}, the ping
        S.kio_backoff = 0;
        eb_connected();
    } else if (event == KRUNKER_EB_MESSAGE) {
        cJSON *o = cJSON_Parse(text);
        if (o) eb_dispatch(o);
        cJSON_Delete(o);
        return; // (eb_dispatch re-rendered)
    } else if (code != 1000) { // onclose: an error and a reconnect, unless closed normally
        kio_fail(S.eb_error[0] ? NULL : "Event-bus connection lost.");
        return;
    } else {
        S.eb_status = EB_DISCONNECTED;
        S.list_requested = 1; // (no automatic reconnect after a normal close, as v10)
    }
    if (S.open) render();
}

void v10_social_account_changed(void) {
    const int64_t id = bus_account();
    const int backend = (int) account_backend();
    if (backend != S.backend) { // the other backend's late answers are dropped
        S.backend = backend;
        g_generation++;
    }
    if (id == S.account_id) return;
    views_clear(); // the profile / clan / post views were the old account's
    eb_reset();
    S.account_id = id;
    if (id && kio()) kio_connect();
    else if (id && backend_social_is_open()) eb_connected();
    for (int i = 0; i < FEED_TABS; i++) S.feeds[i].loaded = 0;
    S.news.loaded = 0;
}

static void conv_open(const int64_t peer) {
    S.selected = peer;
    set_unread(peer, 0);
    for (size_t i = 0; i < sizeof(g_unread_extra) / sizeof(g_unread_extra[0]); i++)
        if (g_unread_extra[i].id == peer) g_unread_extra[i].id = 0;
    eb_send_type("msg:read", "peer", (double) peer);
    S.compose[0] = 0;
    Conv *c = conv_get(peer, 1);
    if (c && !c->initialised && !c->loading) {
        c->loading = 1;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type", "msg:history");
        cJSON_AddNumberToObject(o, "peer", (double) peer);
        cJSON_AddNumberToObject(o, "before", 0);
        cJSON_AddNumberToObject(o, "limit", 15);
        eb_send(o);
    }
}

// v10.0.3's load earlier (deob-10.0.3.js:157767-157783): 25 messages before the oldest, the list's distance from the
// bottom kept, so the messages on screen stay where they are when the page lands above them (render)
static void conv_load_earlier(void) {
    Conv *c = conv_get(S.selected, 0);
    if (!c || !c->has_more || !c->oldest || c->loading) return;
    c->loading = 1;
    const HtmlElem m = html_dom_by_id("v10sMessages");
    HtmlScroll sc;
    S.earlier_anchor = m && html_dom_get_scroll(m, &sc) ? sc.height - sc.top : 0.0f;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "msg:history");
    cJSON_AddNumberToObject(o, "peer", (double) S.selected);
    cJSON_AddNumberToObject(o, "before", (double) c->oldest);
    cJSON_AddNumberToObject(o, "limit", 25);
    eb_send(o);
}

// a JS string's length of UTF-8 text: v10.0.3's canSend is text.length <= 512 and its counter 512 - length
// (deob-10.0.3.js:157704-157705, 158229), in UTF-16 units: a 4-byte sequence (an emoji) is 2. The brain counts the
// same way (social_eb.c utf16_units), so a message the box allows is never refused EB_TEXT_TOO_LONG.
static size_t utf16_len(const char *s) {
    size_t n = 0;
    for (; *s; s++) {
        const unsigned char c = (unsigned char) *s;
        if ((c & 0xC0) != 0x80) n += c >= 0xF0 ? 2 : 1;
    }
    return n;
}

// _50152 send: a pending bubble, then msg:send
static void conv_send(void) {
    char text[sizeof(S.compose)];
    const char *t = S.compose;
    while (*t == ' ' || *t == '\n' || *t == '\t' || *t == '\r') t++;
    size_t len = strlen(t);
    while (len && (t[len - 1] == ' ' || t[len - 1] == '\n' || t[len - 1] == '\t' || t[len - 1] == '\r')) len--;
    const size_t chars = utf16_len(S.compose);
    if (!S.selected || !len || chars > EB_TEXT_MAX || S.eb_status != EB_CONNECTED || S.selected == my_id()) return;
    memcpy(text, t, len);
    text[len] = 0;
    Conv *c = conv_get(S.selected, 1);
    if (!c) return;
    Msg *m = conv_insert(c, c->n);
    if (!m) return;
    m->sent = 1;
    m->text = strdup(text);
    m->ts = now_ms();
    m->status = MSG_PENDING;
    if (c->npending < (int) (sizeof(c->pending) / sizeof(c->pending[0]))) c->pending[c->npending++] = m->local_id;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "msg:send");
    cJSON_AddNumberToObject(o, "to", (double) S.selected);
    cJSON_AddStringToObject(o, "text", text);
    eb_send(o);
    if (!bus_open()) {
        m->status = MSG_FAILED;
        snprintf(m->error, sizeof(m->error), "Not connected.");
        c->npending--;
    }
    S.compose[0] = 0;
}

// ------------------------------------------------------------------ gapi helpers

typedef void (*DoneFn)(int status, const cJSON *data, const cJSON *error, intptr_t tag);
typedef struct {
    DoneFn fn;
    intptr_t tag;
    int generation;
} Pending;
static int g_generation; // bumped when the hub closes / the account changes: late answers are dropped

static void gapi_done(void *user, const int status, const cJSON *data, const cJSON *error) {
    Pending *p = user;
    if (p->generation == g_generation) p->fn(status, data, error, p->tag);
    free(p);
}

static int gapi(const char *method, const char *path, const cJSON *body, const DoneFn fn, const intptr_t tag) {
    if (S.scenario) return 0;
    Pending *p = malloc(sizeof(*p));
    if (!p) return 0;
    p->fn = fn;
    p->tag = tag;
    p->generation = g_generation;
    if (!v10_gapi(method, path, body, gapi_done, p)) {
        free(p);
        return 0;
    }
    return 1;
}

static int ok(const int status, const cJSON *error) { return status >= 200 && status < 300 && !error; }

// ------------------------------------------------------------------ the Social tab: feeds and news

static void feed_done(const int status, const cJSON *data, const cJSON *error, const intptr_t tag) {
    Feed *f = tag < FEED_TABS ? &S.feeds[tag] : &S.news;
    f->loading = 0;
    f->loaded = 1;
    cJSON_Delete(f->posts);
    f->posts = NULL;
    f->error[0] = 0;
    if (!ok(status, error)) {
        snprintf(f->error, sizeof(f->error), "%s", v10_gapi_error_text(error, tag < FEED_TABS ? "Failed to load feed" : "Failed to load news"));
    } else {
        const cJSON *list = cJSON_GetObjectItem(data, tag < FEED_TABS ? "posts" : "news");
        f->posts = cJSON_IsArray(list) ? cJSON_Duplicate(list, 1) : cJSON_CreateArray();
    }
    if (S.open) render();
}

// the panes mount (and fetch) when the Social tab shows
static void feeds_load(const int force) {
    for (int i = 0; i < FEED_TABS; i++) {
        Feed *f = &S.feeds[i];
        if (f->loading || (f->loaded && !force)) continue;
        char path[64];
        snprintf(path, sizeof(path), "/social/feed?tab=%s", k_feed_ids[i]);
        f->loading = gapi("GET", path, NULL, feed_done, i);
        if (!f->loading && !S.scenario) {
            f->loaded = 1;
            snprintf(f->error, sizeof(f->error), "Failed to load feed");
        }
    }
    if (S.sub == 5 && !S.news.loading && (!S.news.loaded || force)) {
        S.news.loading = gapi("GET", "/social/news", NULL, feed_done, FEED_TABS);
        if (!S.news.loading && !S.scenario) {
            S.news.loaded = 1;
            snprintf(S.news.error, sizeof(S.news.error), "Failed to load news");
        }
    }
}

static void sb_window_header(StrBuf *sb, const char *icon, const char *title_esc) {
    sb_appendf(sb, "<div class=\"window-header svelte-4fhlo6\"><i class=\"material-icons window-header-icon svelte-4fhlo6\">%s</i> "
                   "<span class=\"window-header-title svelte-4fhlo6\">%s</span> ", icon, title_esc);
}

// share button _34439 (size small inside posts)
static void sb_share(StrBuf *sb, const char *kind, const char *value, const int small) {
    sb_appendf(sb, "<button class=\"share-btn svelte-1524ito%s\" type=\"button\" title=\"Copy link\" onclick=\"v10s('share','%s',",
               small ? " share-small" : "", kind);
    sb_arg(sb, value);
    sb_append(sb, ")\"><i class=\"material-icons svelte-1524ito\">share</i> <span class=\"svelte-1524ito\">Share</span></button>");
}

// the five feed panes are all mounted (hidden ones too): only the shown pane's cards carry the
// KrunkNative ids, so each id is unique in the document
static int g_post_ids = 1;

// post card _35330 (svelte-10yvzvw); clickable = onOpenDetail set (feeds), not in the detail view
static void sb_post(StrBuf *sb, const cJSON *p, const int clickable) {
    const int64_t pid = (int64_t) jnum(p, "pid");
    const int typ = (int) jnum(p, "typ");
    const char *pln = jstr(p, "pln"), *cln = jstr(p, "cln");
    char time_s[64], dur[48], num[32];
    if (typ == 9 && jnum(p, "exp") > 0) {
        fmt_dur(dur, sizeof(dur), jnum(p, "exp"));
        snprintf(time_s, sizeof(time_s), "ends in %s", dur);
    } else if (jnum(p, "tim") > 0) {
        fmt_dur(dur, sizeof(dur), jnum(p, "tim"));
        snprintf(time_s, sizeof(time_s), "%s ago", dur);
    } else {
        snprintf(time_s, sizeof(time_s), "Just now");
    }
    if (clickable) sb_appendf(sb, "<div class=\"post-card svelte-10yvzvw post-card-clickable\" role=\"button\" tabindex=\"0\" id=\"%sPost%lld\" onclick=\"v10s('post',%lld)\">", g_post_ids ? "v10s" : "v10h", (long long) pid, (long long) pid);
    else sb_append(sb, "<div class=\"post-card svelte-10yvzvw\">");
    // KrunkNative: no profile pictures are hosted, so v10's onerror fallback is the picture
    sb_append(sb, "<div class=\"post-head svelte-10yvzvw\"><img class=\"post-pfp svelte-10yvzvw\" alt=\"\" src=\"./img/player_pfp.png\"> "
                  "<div class=\"post-info svelte-10yvzvw\"><div class=\"post-line-1 svelte-10yvzvw\"><button class=\"post-name svelte-10yvzvw\" type=\"button\" onclick=\"v10s('stop');openPlayerProfile(");
    sb_arg(sb, pln);
    sb_append(sb, ")\">");
    sb_append_esc(sb, pln);
    sb_append(sb, "</button> ");
    if (cln[0]) {
        sb_appendf(sb, "<button class=\"post-clan svelte-10yvzvw\" type=\"button\" style=\"color: %s\" onclick=\"v10s('stop');openClanView(", clan_color(cln, "rgba(255,255,255,0.5)"));
        sb_arg(sb, cln);
        sb_append(sb, ")\">[");
        sb_append_esc(sb, cln);
        sb_append(sb, "]</button> ");
    }
    if (jnum(p, "prm") > 0) sb_append(sb, "<i class=\"material-icons post-badge svelte-10yvzvw\" style=\"color: #fbc02d\">beenhere</i> ");
    if (jnum(p, "ftr") > 0) sb_append(sb, "<i class=\"material-icons post-badge svelte-10yvzvw\" style=\"color: #4fc3f7\">check_circle</i> ");
    commas(num, sizeof(num), jnum(p, "flw"));
    sb_appendf(sb, "<span class=\"post-time svelte-10yvzvw\">%s</span></div> <div class=\"post-sub svelte-10yvzvw\">%s followers</div></div></div> ", time_s, num);
    // media (map / mod / hosted game thumbnails on user-assets)
    const char *lnk = jstr(p, "lnk"), *lnk2 = jstr(p, "lnk2");
    char url[160] = "";
    const char *action = typ == 2 ? "Play it Now!" : typ == 3 ? "Try it Now!" : typ == 7 ? "Join Now!" : NULL;
    if (typ == 2 && lnk[0]) snprintf(url, sizeof(url), "https://user-assets.krunker.io/m%s/thumb.png", lnk);
    else if (typ == 3 && lnk[0]) snprintf(url, sizeof(url), "https://user-assets.krunker.io/md%s/thumb.png", lnk);
    else if (typ == 7 && atoi(lnk2)) snprintf(url, sizeof(url), "https://user-assets.krunker.io/m%d/thumb.png", atoi(lnk2));
    if (url[0]) {
        sb_append(sb, "<div class=\"post-content-media svelte-10yvzvw\"><img class=\"post-content-thumb svelte-10yvzvw\" alt=\"\" loading=\"lazy\" src=\"");
        sb_append_esc(sb, url);
        sb_append(sb, "\" onerror=\"this.style.display='none'\"> ");
        if (action) sb_appendf(sb, "<span class=\"post-content-action svelte-10yvzvw\">%s</span>", action);
        sb_append(sb, "</div> ");
    }
    if (typ == 9 && jnum(p, "fds") > 0) {
        commas(num, sizeof(num), jnum(p, "fds"));
        sb_appendf(sb, "<div class=\"post-giveaway svelte-10yvzvw\"><span class=\"post-giveaway-label svelte-10yvzvw\">Giveaway</span> %s "
                       "<span class=\"post-giveaway-kr svelte-10yvzvw\">KR</span></div> <div class=\"post-giveaway-cta svelte-10yvzvw\">", num);
        const AccountInfo *a = actor();
        if (a && strcmp(a->name, pln) != 0 && !jtrue(p, "_followed")) {
            const int busy = jtrue(p, "_following");
            sb_appendf(sb, "<button class=\"post-follow-btn svelte-10yvzvw\" type=\"button\"%s id=\"%sFollow%lld\" onclick=\"v10s('pfollow',%lld)\">"
                           "<i class=\"material-icons svelte-10yvzvw\">person_add</i> %s</button> <span>and Like to Enter</span>",
                       busy ? " disabled" : "", g_post_ids ? "v10s" : "v10h", (long long) pid, (long long) pid, busy ? "Following..." : "Follow");
        } else if (jtrue(p, "_followed")) {
            sb_append(sb, "<i class=\"material-icons\" style=\"color: #3fd46e; font-size: 16px; vertical-align: middle;\">check</i> <span>Following — Like to Enter</span>");
        } else {
            sb_append(sb, "<span>Follow and Like to Enter</span>");
        }
        sb_append(sb, "</div> ");
    }
    if (typ != 8 && jstr(p, "txt")[0]) {
        sb_append(sb, "<div class=\"post-text svelte-10yvzvw\">");
        sb_append_esc(sb, jstr(p, "txt"));
        sb_append(sb, "</div> ");
    }
    const int voted = (int) jnum(p, "dlk") == 1, logged = actor() != NULL;
    const int busy = jtrue(p, "_busy");
    commas(num, sizeof(num), jnum(p, "vot"));
    sb_appendf(sb, "<div class=\"post-footer svelte-10yvzvw\"><button class=\"vote-btn svelte-10yvzvw%s\" type=\"button\"%s aria-pressed=\"%s\" "
                   "aria-label=\"%s\" title=\"%s\" id=\"%sVote%lld\" onclick=\"v10s('vote',%lld)\"><i class=\"material-icons vote-btn-icon svelte-10yvzvw\">thumb_up</i> "
                   "<span class=\"vote-btn-label svelte-10yvzvw\">%s</span> <span class=\"vote-btn-count svelte-10yvzvw\">%s</span></button> ",
               voted ? " vote-btn-done" : "", busy || !logged ? " disabled" : "", voted ? "true" : "false",
               !logged ? "Log in to like" : voted ? "Unlike post" : "Like post", !logged ? "Log in to like" : voted ? "Unlike" : "Like",
               g_post_ids ? "v10s" : "v10h", (long long) pid, (long long) pid, voted ? "Liked" : "Like", num);
    commas(num, sizeof(num), jnum(p, "cnt"));
    sb_appendf(sb, "<button class=\"comment-btn svelte-10yvzvw\" type=\"button\" onclick=\"v10s('stop');v10s('%s',%lld)\"><i class=\"material-icons comment-count-icon svelte-10yvzvw\">chat_bubble_outline</i> "
                   "<span class=\"comment-count-num svelte-10yvzvw\">%s</span></button> ",
               clickable ? "post" : "noop", (long long) pid, num);
    char pidtxt[24];
    snprintf(pidtxt, sizeof(pidtxt), "%lld", (long long) pid);
    sb_share(sb, "post", pidtxt, 1);
    if (jstr(p, "_voteErr")[0]) {
        sb_append(sb, " <span class=\"vote-error svelte-10yvzvw\">");
        sb_append_esc(sb, jstr(p, "_voteErr"));
        sb_append(sb, "</span>");
    }
    sb_append(sb, "</div></div>");
}

static void sb_social_tab(StrBuf *sb) {
    const AccountInfo *a = actor(); // New post
    sb_append(sb, "<div class=\"social-root svelte-1dyc40r\">");
    sb_window_header(sb, "chat", "Social");
    sb_append(sb, "<div class=\"social-search-form svelte-1dyc40r\"><input type=\"text\" class=\"social-search-input svelte-1dyc40r\" "
                  "placeholder=\"Search players...\" autocomplete=\"off\" spellcheck=\"false\" aria-label=\"Player name search\" id=\"v10sSocialQ\" "
                  "oninput=\"v10sInput('socialq',this.value)\" onkeydown=\"v10sKey('socialq')\" value=\"");
    sb_append_esc(sb, S.social_query);
    sb_append(sb, "\"> <button class=\"social-search-btn svelte-1dyc40r\" type=\"submit\" aria-label=\"Search players\" id=\"v10sSocialGo\" onclick=\"v10s('socialsearch')\">"
                  "<i class=\"material-icons social-search-icon svelte-1dyc40r\">search</i></button></div> ");
    if (a && a->level >= 10)
        sb_append(sb, "<button class=\"social-compose-btn svelte-1dyc40r\" type=\"button\" id=\"v10sCompose\" onclick=\"v10s('compose')\"><i class=\"material-icons social-compose-icon svelte-1dyc40r\">edit</i> "
                      "<span class=\"social-compose-label svelte-1dyc40r\">New post</span></button>");
    sb_append(sb, "</div> <div class=\"social-tabs svelte-1dyc40r\"><div class=\"simple-tabs svelte-1oqd18e\" role=\"tablist\">");
    for (int i = 0; i < 6; i++)
        sb_appendf(sb, "<div class=\"simple-tab svelte-1oqd18e%s\" role=\"tab\" aria-selected=\"%s\" tabindex=\"0\" id=\"v10sSub%s\" "
                       "onmouseenter=\"playTick(0.1)\" onclick=\"playSelect(0.1);v10s('sub',%d)\"><i class=\"material-icons simple-tab-icon svelte-1oqd18e\">%s</i> "
                       "<span class=\"simple-tab-label svelte-1oqd18e\">%s</span></div>",
                   S.sub == i ? " simple-tab-active" : "", S.sub == i ? "true" : "false", k_social_tabs[i].id, i, k_social_tabs[i].icon,
                   k_social_tabs[i].label);
    sb_append(sb, "</div></div> <div class=\"social-content svelte-1dyc40r\">");
    for (int i = 0; i < FEED_TABS; i++) {
        const Feed *f = &S.feeds[i];
        sb_appendf(sb, "<div class=\"social-pane svelte-1dyc40r%s\"><div class=\"feed-tab svelte-1dmc7se\" id=\"v10sFeed%d\">",
                   S.sub == i ? "" : " social-pane-hidden", i);
        if (f->loading || !f->loaded) sb_append(sb, "<div class=\"feed-status svelte-1dmc7se\">Loading…</div>");
        else if (f->error[0]) {
            sb_append(sb, "<div class=\"feed-status feed-error svelte-1dmc7se\">");
            sb_append_esc(sb, f->error);
            sb_append(sb, "</div>");
        } else if (cJSON_GetArraySize(f->posts) == 0) {
            sb_appendf(sb, "<div class=\"feed-status svelte-1dmc7se\">%s</div>", k_social_tabs[i].empty);
        } else {
            const cJSON *p;
            g_post_ids = S.sub == i;
            cJSON_ArrayForEach(p, f->posts) sb_post(sb, p, 1);
            g_post_ids = 1;
        }
        sb_append(sb, "</div></div>");
    }
    if (S.sub == 5) {
        sb_append(sb, " <div class=\"social-pane svelte-1dyc40r\"><div class=\"news-tab svelte-1ult4vg\">");
        if (S.news.loading || !S.news.loaded) sb_append(sb, "<div class=\"news-status svelte-1ult4vg\">Loading…</div>");
        else if (S.news.error[0]) {
            sb_append(sb, "<div class=\"news-status news-error svelte-1ult4vg\">");
            sb_append_esc(sb, S.news.error);
            sb_append(sb, "</div>");
        } else if (cJSON_GetArraySize(S.news.posts) == 0) {
            sb_append(sb, "<div class=\"news-status svelte-1ult4vg\">No news today.</div>");
        } else {
            sb_append(sb, "<div class=\"news-list svelte-1ult4vg\">");
            const cJSON *n;
            cJSON_ArrayForEach(n, S.news.posts) {
                if (!cJSON_IsString(n) || !n->valuestring[0]) continue;
                sb_append(sb, "<div class=\"news-item svelte-1ult4vg\">");
                sb_append(sb, n->valuestring); // {@html}: the brain's own news (accounts.news)
                sb_append(sb, "</div>");
            }
            sb_append(sb, "</div>");
        }
        sb_append(sb, "</div></div>");
    }
    sb_append(sb, "</div></div>");
}

// ------------------------------------------------------------------ the Friends tab

static void sb_avatar(StrBuf *sb, const char *name) {
    // _34602: premium players' pictures live on user-assets; KrunkNative hosts none, so the
    // fallback picture (v10's onerror result)
    sb_append(sb, "<img class=\"player-avatar-img svelte-1gi5a96\" loading=\"lazy\" src=\"./img/player_pfp.png\" alt=\"");
    sb_append_esc(sb, name);
    sb_append(sb, "\">");
}

static int friend_active(const cJSON *f) { return jnum(f, "unread") > 0 || jnum(f, "lastTs") > 0; }

static int friend_cmp(const void *pa, const void *pb) {
    const cJSON *a = *(const cJSON *const *) pa, *b = *(const cJSON *const *) pb;
    const int aa = friend_active(a), ba = friend_active(b);
    if (aa != ba) return aa ? -1 : 1;
    const int ao = jtrue(a, "online"), bo = jtrue(b, "online");
    if (ao != bo) return ao ? -1 : 1;
    if (jnum(a, "unread") != jnum(b, "unread")) return jnum(b, "unread") > jnum(a, "unread") ? 1 : -1;
    if (jnum(a, "lastTs") != jnum(b, "lastTs")) return jnum(b, "lastTs") > jnum(a, "lastTs") ? 1 : -1;
    return strcasecmp(jstr(a, "name"), jstr(b, "name"));
}

static int ci_contains(const char *hay, const char *needle) {
    if (!needle[0]) return 1;
    const size_t n = strlen(needle);
    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, n)) return 1;
    return 0;
}

static void last_preview(const cJSON *f, char *out, const size_t n) {
    const Conv *c = conv_get((int64_t) jnum(f, "id"), 0);
    const Msg *m = c && c->n ? &c->msgs[c->n - 1] : NULL;
    const char *text = m ? m->text : jstr(f, "lastText");
    if (!text || !text[0]) {
        snprintf(out, n, "No messages yet");
        return;
    }
    const int mine = m ? m->sent : jtrue(f, "lastFromMe");
    // 40 characters (code points) then "…"
    size_t bytes = 0, chars = 0;
    while (text[bytes] && chars < 40) {
        bytes++;
        while (((unsigned char) text[bytes] & 0xC0) == 0x80) bytes++;
        chars++;
    }
    snprintf(out, n, "%s%.*s%s", mine ? "You: " : "", (int) bytes, text, text[bytes] ? "…" : "");
}

static void sb_friend_list(StrBuf *sb) {
    sb_append(sb, "<div class=\"friend-list svelte-e2g8p2\">");
    const int n = cJSON_GetArraySize(S.friends);
    if (n == 0) {
        if (S.eb_status == EB_CONNECTING || S.friends_loading || (S.eb_status == EB_CONNECTED && !S.friends))
            sb_append(sb, "<div class=\"friend-list-empty svelte-e2g8p2\"><i class=\"material-icons spin svelte-e2g8p2\">hourglass_empty</i> <span class=\"svelte-e2g8p2\">Loading…</span></div>");
        else if (S.eb_status == EB_ERROR)
            sb_append(sb, "<div class=\"friend-list-empty friend-list-error svelte-e2g8p2\"><i class=\"material-icons svelte-e2g8p2\">cloud_off</i> <span class=\"svelte-e2g8p2\">Disconnected</span></div>");
        else
            sb_append(sb, "<div class=\"friend-list-empty svelte-e2g8p2\"><i class=\"material-icons svelte-e2g8p2\">sentiment_neutral</i> <span class=\"svelte-e2g8p2\">No friends yet</span></div>");
        sb_append(sb, "</div>");
        return;
    }
    const cJSON **sorted = calloc((size_t) n, sizeof(*sorted));
    int k = 0;
    const cJSON *f;
    cJSON_ArrayForEach(f, S.friends) if (sorted && ci_contains(jstr(f, "name"), S.friend_filter)) sorted[k++] = f;
    if (sorted) qsort(sorted, (size_t) k, sizeof(*sorted), friend_cmp);
    sb_append(sb, "<div class=\"friend-list-scroll svelte-e2g8p2\" id=\"v10sFriendScroll\">");
    for (int i = 0; i < k; i++) {
        f = sorted[i];
        const int64_t id = (int64_t) jnum(f, "id");
        const int unread = (int) jnum(f, "unread"), online = jtrue(f, "online");
        char prev[256];
        last_preview(f, prev, sizeof(prev));
        sb_appendf(sb, "<button class=\"friend-item svelte-e2g8p2%s\" aria-label=\"Chat with ", S.selected == id ? " selected" : "");
        sb_append_esc(sb, jstr(f, "name"));
        sb_appendf(sb, "\" id=\"v10sFriend%lld\" onclick=\"v10s('friend',%lld)\"><div class=\"friend-avatar svelte-e2g8p2\">", (long long) id, (long long) id);
        sb_avatar(sb, jstr(f, "name"));
        sb_appendf(sb, " <span class=\"friend-presence-dot svelte-e2g8p2 %s\"></span></div> <div class=\"friend-text svelte-e2g8p2\"><div class=\"friend-name-row svelte-e2g8p2\">"
                       "<span class=\"friend-name svelte-e2g8p2\">", online ? "online" : "offline");
        sb_append_esc(sb, jstr(f, "name"));
        sb_append(sb, "</span> ");
        if (unread > 0) {
            if (unread > 99) sb_append(sb, "<span class=\"friend-unread svelte-e2g8p2\">99+</span>");
            else sb_appendf(sb, "<span class=\"friend-unread svelte-e2g8p2\">%d</span>", unread);
        }
        sb_append(sb, "</div> <div class=\"friend-preview svelte-e2g8p2\">");
        sb_append_esc(sb, prev);
        sb_append(sb, "</div></div></button>");
    }
    free(sorted);
    if (k == 0 && S.friend_filter[0]) sb_append(sb, "<div class=\"friend-list-empty svelte-e2g8p2\"><span class=\"svelte-e2g8p2\">No matches</span></div>");
    if (S.friends_more && !S.friend_filter[0])
        sb_appendf(sb, "<button class=\"friend-list-more svelte-e2g8p2\"%s onclick=\"v10s('more')\">%s</button>", S.friends_loading ? " disabled" : "",
                   S.friends_loading ? "<i class=\"material-icons spin svelte-e2g8p2\">hourglass_empty</i> <span>Loading…</span>" : "<span>Load more</span>");
    sb_append(sb, "</div> <div class=\"friend-list-search svelte-e2g8p2\"><i class=\"material-icons svelte-e2g8p2\">search</i> "
                  "<input type=\"text\" placeholder=\"Search friends\" aria-label=\"Search friends\" class=\"svelte-e2g8p2\" id=\"v10sFriendQ\" "
                  "oninput=\"v10sInput('friendq',this.value)\" value=\"");
    sb_append_esc(sb, S.friend_filter);
    sb_append(sb, "\"></div></div>");
}

static void sb_conversation(StrBuf *sb) {
    sb_append(sb, "<div class=\"conversation-root svelte-128yn5i\">");
    const cJSON *f = S.selected ? friend_by_id(S.selected) : NULL;
    if (!f) {
        sb_append(sb, "<div class=\"conversation-empty svelte-128yn5i\"><i class=\"material-icons conversation-empty-icon svelte-128yn5i\">forum</i> "
                      "<div class=\"conversation-empty-title svelte-128yn5i\">Select a friend</div> <div class=\"conversation-empty-subtitle svelte-128yn5i\">"
                      "Pick someone from your friends list to start chatting.</div></div></div>");
        return;
    }
    const char *name = jstr(f, "name"), *game = jstr(f, "gameInstanceId");
    const int online = jtrue(f, "online");
    sb_append(sb, "<div class=\"conversation-header svelte-128yn5i\"><button type=\"button\" class=\"conversation-header-profile svelte-128yn5i\" id=\"v10sConvProfile\" onclick=\"openPlayerProfile(");
    sb_arg(sb, name);
    sb_append(sb, ")\"><div class=\"conversation-header-avatar svelte-128yn5i\">");
    sb_avatar(sb, name);
    sb_appendf(sb, " <span class=\"conversation-presence-dot svelte-128yn5i %s\"></span></div> <div class=\"conversation-header-info svelte-128yn5i\">"
                   "<div class=\"conversation-header-name svelte-128yn5i\">", online ? "online" : "offline");
    sb_append_esc(sb, name);
    sb_appendf(sb, "</div> <div class=\"conversation-header-status svelte-128yn5i%s%s\">%s</div></div></button> ", online ? " online" : "",
               game[0] ? " ingame" : "", game[0] ? "In a match" : online ? "Online" : "Offline");
    if (online && game[0]) {
        const int locked = jtrue(f, "gamePasswordProtected");
        sb_append(sb, "<button type=\"button\" class=\"conversation-header-join svelte-128yn5i\" title=\"Join ");
        sb_append_esc(sb, name);
        sb_appendf(sb, "'s match%s\" id=\"v10sJoin\" onclick=\"v10s('join',", locked ? " (password required)" : "");
        sb_arg(sb, game);
        sb_appendf(sb, ")\"><i class=\"material-icons svelte-128yn5i\">%s</i> <span>Join</span></button>", locked ? "lock" : "sports_esports");
    }
    sb_append(sb, "</div> <div class=\"conversation-messages svelte-128yn5i\" id=\"v10sMessages\">");
    const Conv *c = conv_get(S.selected, 0);
    if (c && c->has_more)
        sb_appendf(sb, "<button type=\"button\" class=\"conversation-load-earlier svelte-128yn5i\"%s id=\"v10sEarlier\" onclick=\"v10s('earlier')\">%s</button> ",
                   c->loading ? " disabled" : "",
                   c->loading ? "<i class=\"material-icons spin svelte-128yn5i\">hourglass_empty</i> <span>Loading…</span>"
                              : "<i class=\"material-icons svelte-128yn5i\">expand_less</i> <span>Load earlier messages</span>");
    else if (c && c->initialised && c->n > 0)
        sb_append(sb, "<div class=\"conversation-history-end svelte-128yn5i\">Start of conversation</div> ");
    if (!c || c->n == 0) {
        if (c && c->initialised && !c->loading)
            sb_append(sb, "<div class=\"conversation-no-messages svelte-128yn5i\"><i class=\"material-icons svelte-128yn5i\">chat_bubble_outline</i> "
                          "<span class=\"svelte-128yn5i\">No messages yet — say hi.</span></div>");
        else
            sb_append(sb, "<div class=\"conversation-no-messages svelte-128yn5i\"><i class=\"material-icons spin svelte-128yn5i\">hourglass_empty</i> "
                          "<span class=\"svelte-128yn5i\">Loading…</span></div>");
    } else {
        int last_y = -1, last_d = -1;
        for (int i = 0; i < c->n; i++) {
            const Msg *m = &c->msgs[i];
            struct tm tm;
            local_tm(m->ts, &tm);
            if (tm.tm_year != last_y || tm.tm_yday != last_d) {
                char day[64];
                fmt_day(day, sizeof(day), m->ts);
                sb_appendf(sb, "<div class=\"conversation-day-divider svelte-128yn5i\"><span class=\"svelte-128yn5i\">%s</span></div>", day);
                last_y = tm.tm_year;
                last_d = tm.tm_yday;
            }
            char full[64], clock[32];
            fmt_full(full, sizeof(full), m->ts);
            fmt_clock(clock, sizeof(clock), m->ts);
            sb_appendf(sb, "<div class=\"bubble-row svelte-jj9ljg %s\" title=\"%s\"><div class=\"bubble svelte-jj9ljg%s%s\"><div class=\"bubble-text svelte-jj9ljg\">",
                       m->sent ? "sent" : "received", full, m->status == MSG_PENDING ? " pending" : "", m->status == MSG_FAILED ? " failed" : "");
            sb_append_esc(sb, m->text ? m->text : ""); // :emoji: codes stay text (no emoji skins are served)
            sb_appendf(sb, "</div> <div class=\"bubble-meta svelte-jj9ljg\"><span class=\"bubble-time svelte-jj9ljg\">%s</span> ", clock);
            if (m->status == MSG_PENDING) sb_append(sb, "<i class=\"material-icons bubble-status-icon svelte-jj9ljg\" title=\"Sending…\">schedule</i>");
            else if (m->status == MSG_FAILED) {
                sb_append(sb, "<i class=\"material-icons bubble-status-icon bubble-status-failed svelte-jj9ljg\" title=\"");
                sb_append_esc(sb, m->error[0] ? m->error : "Failed");
                sb_append(sb, "\">error_outline</i>");
            } else if (m->sent) sb_append(sb, "<i class=\"material-icons bubble-status-icon bubble-status-ok svelte-jj9ljg\" title=\"Delivered\">done</i>");
            sb_append(sb, "</div> ");
            if (m->status == MSG_FAILED && m->error[0]) {
                sb_append(sb, "<div class=\"bubble-error svelte-jj9ljg\">");
                sb_append_esc(sb, m->error);
                sb_append(sb, "</div>");
            }
            sb_append(sb, "</div></div>");
        }
    }
    const size_t chars = utf16_len(S.compose);
    const int left = EB_TEXT_MAX - (int) chars;
    const char *t = S.compose;
    while (*t == ' ' || *t == '\n' || *t == '\t') t++;
    const int can_send = *t && chars <= EB_TEXT_MAX && S.eb_status == EB_CONNECTED;
    const int connected = S.eb_status == EB_CONNECTED;
    sb_appendf(sb, "</div> <div class=\"conversation-compose svelte-128yn5i\"><div class=\"conversation-input-row svelte-128yn5i\">"
                   "<textarea class=\"conversation-input svelte-128yn5i\" placeholder=\"Type a message… (Enter to send, : for emoji)\" rows=\"2\" "
                   "aria-label=\"Message input\" maxlength=\"512\" id=\"v10sMsgInput\"%s oninput=\"v10sInput('msg',this.value)\" onkeydown=\"v10sKey('msg')\">",
               connected ? "" : " disabled");
    sb_append_esc(sb, S.compose);
    sb_appendf(sb, "</textarea> <button class=\"conversation-emoji-button svelte-128yn5i\" aria-label=\"Emoji\" title=\"Emoji\"%s onclick=\"v10s('emoji')\">"
                   "<i class=\"material-icons svelte-128yn5i\">mood</i></button> <button class=\"conversation-send-button svelte-128yn5i\" aria-label=\"Send message\" "
                   "id=\"v10sSend\"%s onclick=\"v10s('send')\"><i class=\"material-icons svelte-128yn5i\">send</i></button></div> "
                   "<div class=\"conversation-input-meta svelte-128yn5i\"><span class=\"conversation-char-count svelte-128yn5i%s\">%d</span></div></div></div>",
               connected ? "" : " disabled", can_send ? "" : " disabled", left <= 32 ? " conversation-char-count-low" : "", left);
}

static void sb_friends_tab(StrBuf *sb) {
    char title[48];
    snprintf(title, sizeof(title), "Friends (%d)", cJSON_GetArraySize(S.friends));
    const char *label = S.eb_status == EB_CONNECTING ? "Connecting…" : S.eb_status == EB_CONNECTED ? "Connected"
                        : S.eb_status == EB_ERROR ? "Disconnected" : "Offline";
    sb_append(sb, "<div class=\"friends-root svelte-u33r8v\">");
    sb_window_header(sb, "chat", title);
    sb_appendf(sb, "<span class=\"friends-status svelte-u33r8v%s%s\" id=\"v10sEbStatus\"><span class=\"friends-status-dot svelte-u33r8v\"></span> %s</span></div> ",
               S.eb_status == EB_CONNECTED ? " friends-status-ok" : "", S.eb_status == EB_ERROR ? " friends-status-error" : "", label);
    if (S.eb_error[0]) {
        sb_append(sb, "<div class=\"friends-error-banner svelte-u33r8v\"><i class=\"material-icons svelte-u33r8v\">error_outline</i> <span class=\"svelte-u33r8v\">");
        sb_append_esc(sb, S.eb_error);
        sb_append(sb, "</span></div> ");
    }
    sb_append(sb, "<div class=\"friends-body svelte-u33r8v\"><aside class=\"friends-sidebar svelte-u33r8v\">");
    sb_friend_list(sb);
    sb_append(sb, "</aside> <main class=\"friends-main svelte-u33r8v\">");
    sb_conversation(sb);
    sb_append(sb, "</main></div></div>");
}

// ------------------------------------------------------------------ the Search tab

static void search_done(const int status, const cJSON *data, const cJSON *error, const intptr_t tag) {
    if (tag != S.search_seq) return;
    S.search_loading = 0;
    cJSON_Delete(S.results);
    S.results = NULL;
    S.search_error[0] = 0;
    if (!ok(status, error)) snprintf(S.search_error, sizeof(S.search_error), "%s", v10_gapi_error_text(error, "Search failed"));
    else S.results = cJSON_Duplicate(cJSON_GetObjectItem(data, "results"), 1);
    if (S.open) render();
}

static void search_fire(void *user) {
    (void) user;
    S.search_timer = 0;
    const char *q = S.search_q;
    while (*q == ' ') q++;
    if (!*q) return;
    S.search_seq++;
    S.search_loading = 1;
    S.search_error[0] = 0;
    S.searched = 1;
    char enc[256], path[320];
    backend_url_encode(enc, sizeof(enc), q);
    snprintf(path, sizeof(path), "/players/search?q=%s&limit=20", enc);
    if (!gapi("GET", path, NULL, search_done, S.search_seq)) {
        S.search_loading = 0;
        snprintf(S.search_error, sizeof(S.search_error), "Search failed");
    }
    if (S.open) render();
}

// the query changed: 300 ms debounce
static void search_changed(void) {
    if (S.search_timer) html_timer_cancel(S.search_timer);
    S.search_timer = 0;
    const char *q = S.search_q;
    while (*q == ' ') q++;
    if (!*q) {
        cJSON_Delete(S.results);
        S.results = NULL;
        S.search_error[0] = 0;
        S.searched = S.search_loading = 0;
        return;
    }
    S.search_timer = html_timer_add(300, search_fire, NULL);
}

static void sb_search_tab(StrBuf *sb) {
    sb_append(sb, "<div class=\"profile-search-root svelte-fo44ss\">");
    sb_window_header(sb, "person_search", "Player Profiles");
    sb_append(sb, "</div> <div class=\"profile-search-content svelte-fo44ss\" id=\"v10sSearchContent\"><div class=\"profile-search-form svelte-fo44ss\"><i class=\"material-icons profile-search-icon svelte-fo44ss\">search</i> "
                  "<input type=\"text\" class=\"profile-search-input svelte-fo44ss\" placeholder=\"Search username...\" autocomplete=\"off\" spellcheck=\"false\" "
                  "aria-label=\"Player name search\" id=\"v10sSearchQ\" oninput=\"v10sInput('search',this.value)\" value=\"");
    sb_append_esc(sb, S.search_q);
    sb_append(sb, "\"></div> ");
    if (S.search_error[0]) {
        sb_append(sb, "<div class=\"profile-search-error svelte-fo44ss\">");
        sb_append_esc(sb, S.search_error);
        sb_append(sb, "</div> ");
    }
    if (S.search_loading) {
        sb_append(sb, "<div class=\"profile-search-loading svelte-fo44ss\"><i class=\"material-icons spin svelte-fo44ss\">hourglass_empty</i></div>");
    } else if (S.searched && !S.search_error[0]) {
        if (cJSON_GetArraySize(S.results) == 0) {
            sb_append(sb, "<div class=\"profile-search-empty svelte-fo44ss\">No players found.</div>");
        } else {
            sb_append(sb, "<div class=\"profile-search-results svelte-fo44ss\">");
            const cJSON *r;
            int i = 0;
            cJSON_ArrayForEach(r, S.results) {
                const char *name = jstr(r, "player_name");
                char score[32];
                commas(score, sizeof(score), jnum(r, "player_score"));
                sb_appendf(sb, "<div class=\"player-card svelte-dp3rf9\" role=\"button\" tabindex=\"0\" id=\"v10sResult%d\" onclick=\"openPlayerProfile(", i++);
                sb_arg(sb, name);
                sb_append(sb, ")\"><div class=\"player-card-left svelte-dp3rf9\">");
                if (jnum(r, "player_premium") > 0) sb_append(sb, "<i class=\"material-icons player-card-premium svelte-dp3rf9\" title=\"Premium\">beenhere</i> ");
                if (jtrue(r, "player_featured")) sb_append(sb, "<i class=\"material-icons player-card-featured svelte-dp3rf9\" title=\"Featured\">check_circle</i> ");
                sb_appendf(sb, "<span class=\"player-card-name svelte-dp3rf9\" style=\"color: %s;\">", jtrue(r, "player_hack") ? "#f44336" : "#ffffff");
                sb_append_esc(sb, name);
                sb_append(sb, "</span> ");
                if (jstr(r, "player_clan")[0]) {
                    sb_append(sb, "<span class=\"player-card-clan svelte-dp3rf9\">[");
                    sb_append_esc(sb, jstr(r, "player_clan"));
                    sb_append(sb, "]</span>");
                }
                sb_appendf(sb, "</div> <div class=\"player-card-right svelte-dp3rf9\"><span class=\"player-card-score svelte-dp3rf9\">%s</span></div></div>", score);
            }
            sb_append(sb, "</div>");
        }
    } else if (!S.searched) {
        sb_append(sb, "<div class=\"profile-search-hint svelte-fo44ss\">Search for a player by name to view their profile.</div>");
    }
    sb_append(sb, "</div></div>");
}

// ------------------------------------------------------------------ the view stack (_5216)

static View *top_view(void) { return S.nviews ? &S.views[S.nviews - 1] : NULL; }

static void view_free(View *v) {
    cJSON_Delete(v->data);
    cJSON_Delete(v->sales);
    cJSON_Delete(v->listings);
    cJSON_Delete(v->feed);
    cJSON_Delete(v->reports);
    cJSON_Delete(v->followers);
    cJSON_Delete(v->following);
    free(v->cskins);
    if (v->type == V_PROFILE && v->tab == 3) ui_tooltip_cancel(); // its badge tooltip goes with it (deob-10.0.3.js:126018-126020)
    memset(v, 0, sizeof(*v));
}

// Every view here is one entry of the page's single v10 view stack (v10_view_push in v10_ui.h,
// v10_competitive.c): it draws the overlay / wrapper and pops on a backdrop click; this module keeps
// each view's state and markup (v10_view_set while it is the top entry).
static void view_closed(const int accepted, void *user) { // the shared stack popped one of ours
    (void) accepted;
    const int depth = (int) (intptr_t) user;
    while (S.nviews && S.views[S.nviews - 1].depth >= depth) view_free(&S.views[--S.nviews]);
}

static void view_rerender(void) { render_views(); }

static void view_pop(void);

static View *view_push(const int type) {
    if (S.nviews == MAX_VIEWS) view_pop();
    v10_view_push(strdup(""), view_rerender, view_closed, (void *) (intptr_t) (v10_view_depth() + 1));
    View *v = &S.views[S.nviews++];
    memset(v, 0, sizeof(*v));
    v->type = type;
    v->depth = v10_view_depth();
    return v;
}

static void view_pop(void) {
    View *v = top_view();
    if (!v) return;
    if (v->depth == v10_view_depth()) {
        v10_view_pop(0); // view_closed frees it
        return;
    }
    view_free(&S.views[--S.nviews]);
}

static void views_clear(void) {
    while (S.nviews) view_pop();
}

// the stack entry a gapi answer belongs to: the tag is (index << 8 | type); stale entries are dropped
static intptr_t view_tag(const View *v) { return (intptr_t) (((v - S.views) << 8) | v->type); }
static View *view_of(const intptr_t tag) {
    const int i = (int) (tag >> 8);
    if (i < 0 || i >= S.nviews || S.views[i].type != (int) (tag & 0xff)) return NULL;
    return &S.views[i];
}

// ---- player profile (_34510)

static const char *const k_profile_tabs[8] = {"Stats", "Feed", "Sales", "Badges", "Titles", "Creations", "Match History", "Reports"};
// v10 REPORT_REASONS (v10.js _7525): the reporter's select; a report stores the index or the name
static const char *const k_report_reasons[7] = {"Hacking", "Exploiting", "Scamming", "Harassment", "Other", "Boosting", "Market Manipulation"};

static void profile_feed_done(const int status, const cJSON *data, const cJSON *error, const intptr_t tag) {
    View *v = view_of(tag);
    if (!v) return;
    v->feed_loading = 0;
    cJSON_Delete(v->feed);
    v->feed = ok(status, error) ? cJSON_Duplicate(cJSON_GetObjectItem(data, "posts"), 1) : cJSON_CreateArray();
    render_views();
}

// the Reports tab (developers and moderators; v10 getReports, GET /players/<name>/reports): loaded when
// the tab opens; the rows are the brain's {rp_id, rp_date, rp_reportername, rp_index, rp_text}
static void profile_reports_done(const int status, const cJSON *data, const cJSON *error, const intptr_t tag) {
    View *v = view_of(tag);
    if (!v) return;
    v->reports_loading = 0;
    cJSON_Delete(v->reports);
    v->reports = ok(status, error) && cJSON_IsArray(data) ? cJSON_Duplicate(data, 1) : cJSON_CreateArray();
    render_views();
}

static void profile_reports_load(View *v) {
    char enc[160], path[200];
    backend_url_encode(enc, sizeof(enc), jstr(v->data, "player_name"));
    snprintf(path, sizeof(path), "/players/%s/reports", enc);
    v->reports_loading = gapi("GET", path, NULL, profile_reports_done, view_tag(v));
    if (!v->reports_loading) v->reports = cJSON_CreateArray();
    render_views();
}

static void profile_done(const int status, const cJSON *data, const cJSON *error, const intptr_t tag) {
    View *v = view_of(tag);
    if (!v) return;
    v->loading = 0;
    if (!ok(status, error) || !data) {
        snprintf(v->error, sizeof(v->error), "%s", v10_gapi_error_text(error, "Profile not found"));
    } else {
        v->data = cJSON_Duplicate(data, 1);
        // player_stats is an object or its JSON text (v10 parses the text, deob-10.0.3.js:125746-125761)
        const cJSON *st = cJSON_GetObjectItem(v->data, "player_stats");
        cJSON *parsed = cJSON_IsString(st) ? cJSON_Parse(st->valuestring) : NULL;
        if (cJSON_IsObject(parsed)) cJSON_ReplaceItemInObject(v->data, "player_stats", parsed);
        else cJSON_Delete(parsed);
        char path[96];
        snprintf(path, sizeof(path), "/social/feed?player_id=%lld&limit=20", (long long) jnum(data, "player_id"));
        v->feed_loading = gapi("GET", path, NULL, profile_feed_done, tag);
    }
    render_views();
}

static void open_profile(const char *name) {
    if (!name || !name[0]) return;
    View *v = view_push(V_PROFILE);
    snprintf(v->name, sizeof(v->name), "%s", name);
    v->loading = 1;
    v->sales_range = 3;
    char enc[160], path[200];
    backend_url_encode(enc, sizeof(enc), name);
    snprintf(path, sizeof(path), "/players/%s", enc);
    if (!gapi("GET", path, NULL, profile_done, view_tag(v)) && !S.scenario) {
        v->loading = 0;
        snprintf(v->error, sizeof(v->error), "Failed to load profile.");
    }
    render_views();
}

static void stat_card(StrBuf *sb, const char *label, const char *value, const int wide) {
    sb_appendf(sb, "<div class=\"profile-detail-stat%s svelte-1hghrmf\"><div class=\"profile-detail-stat-label svelte-1hghrmf\">%s</div> "
                   "<div class=\"profile-detail-stat-value svelte-1hghrmf\">%s</div></div> ",
               wide ? " profile-detail-stat-wide" : "", label, value);
}

// fmt: comma thousands on the integer part (1234.5 -> 1,234.5)
static void fmt_num(char *out, const size_t n, const double v) { commas(out, n, floor(v)); }

static void fixed_trim(char *out, const size_t n, const double v, const int dp) {
    snprintf(out, n, "%.*f", dp, v); // parseFloat(x.toFixed(dp))
    if (strchr(out, '.')) {
        size_t l = strlen(out);
        while (l && out[l - 1] == '0') out[--l] = 0;
        if (l && out[l - 1] == '.') out[--l] = 0;
    }
}

static double stat_of(const cJSON *stats, const char *k) { return jnum(stats, k); }

static void sb_profile_stats(StrBuf *sb, const cJSON *d) {
    const cJSON *st = cJSON_GetObjectItem(d, "player_stats");
    const double score = jnum(d, "player_score"), kills = jnum(d, "player_kills"), deaths = jnum(d, "player_deaths");
    const double games = jnum(d, "player_games_played"), wins = jnum(d, "player_wins");
    char v[64];
    sb_append(sb, "<div class=\"profile-detail-section-header svelte-1hghrmf\">General</div> <div class=\"profile-detail-stat-grid svelte-1hghrmf\">");
    snprintf(v, sizeof(v), "%d", level_of(score)); stat_card(sb, "LVL", v, 0);
    fmt_num(v, sizeof(v), jnum(d, "player_funds")); stat_card(sb, "KR", v, 0);
    fmt_num(v, sizeof(v), jnum(d, "player_elo4")); stat_card(sb, "Junk", v, 0);
    fmt_num(v, sizeof(v), jnum(d, "player_skinvalue")); stat_card(sb, "Inventory ~", v, 0);
    fmt_num(v, sizeof(v), score); stat_card(sb, "Score", v, 0);
    fixed_trim(v, sizeof(v), score / (kills > 1 ? kills : 1), 2); stat_card(sb, "SPK", v, 0);
    fmt_num(v, sizeof(v), kills); stat_card(sb, "Kills", v, 0);
    fmt_num(v, sizeof(v), deaths); stat_card(sb, "Deaths", v, 0);
    fixed_trim(v, sizeof(v), kills / (deaths > 1 ? deaths : 1), 4); stat_card(sb, "K/D", v, 0);
    fixed_trim(v, sizeof(v), kills / (games > 1 ? games : 1), 4); stat_card(sb, "KPG", v, 0);
    fmt_num(v, sizeof(v), games); stat_card(sb, "Games", v, 0);
    fixed_trim(v, sizeof(v), wins / (games - wins > 1 ? games - wins : 1), 3); stat_card(sb, "W/L", v, 0);
    fmt_num(v, sizeof(v), wins); stat_card(sb, "Wins", v, 0);
    fmt_num(v, sizeof(v), games - wins); stat_card(sb, "Losses", v, 0);
    static const struct { const char *label, *key; } counted[] = {
        {"Assists", "ast"}, {"Melee Kills", "mk"}, {"Fist Kills", "fk"}, {"Thrown Kills", "tmk"},
        {"Headshots", "hs"}, {"Legshots", "ls"}, {"Wallbangs", "wb"},
    };
    for (size_t i = 0; i < sizeof(counted) / sizeof(counted[0]); i++) {
        fmt_num(v, sizeof(v), stat_of(st, counted[i].key));
        stat_card(sb, counted[i].label, v, 0);
    }
    const double shots = stat_of(st, "s"), hits = fmin(shots, stat_of(st, "h"));
    fmt_num(v, sizeof(v), shots); stat_card(sb, "Shots", v, 0);
    fmt_num(v, sizeof(v), hits); stat_card(sb, "Hits", v, 0);
    fmt_num(v, sizeof(v), shots - hits); stat_card(sb, "Misses", v, 0);
    char acc[32];
    fixed_trim(acc, sizeof(acc), shots > 0 ? hits / shots * 100.0 : 0, 2);
    snprintf(v, sizeof(v), "%s%%", acc); stat_card(sb, "Accuracy", v, 0);
    fmt_dur(v, sizeof(v), jnum(d, "player_timeplayed"));
    sb_appendf(sb, "<div class=\"profile-detail-stat profile-detail-stat-wide svelte-1hghrmf\"><div class=\"profile-detail-stat-label svelte-1hghrmf\">Time Played</div> "
                   "<div class=\"profile-detail-stat-value svelte-1hghrmf\">%s</div></div></div> ", v);
    sb_append(sb, "<div class=\"profile-detail-section-header svelte-1hghrmf\">Killstreaks</div> <div class=\"profile-detail-stat-grid svelte-1hghrmf\">");
    static const struct { const char *label, *key; } streaks[] = {
        {"Nukes", "n"}, {"Airdrops", "ad"}, {"Airdrops Stolen", "sad"}, {"Slimers", "sl"}, {"Juggernauts", "jg"},
        {"Juggernauts Killed", "kjg"}, {"War Machines", "wm"},
    };
    for (size_t i = 0; i < sizeof(streaks) / sizeof(streaks[0]); i++) {
        fmt_num(v, sizeof(v), stat_of(st, streaks[i].key));
        stat_card(sb, streaks[i].label, v, 0);
    }
    sb_append(sb, "</div> <div class=\"profile-detail-section-header svelte-1hghrmf\">Other</div> <div class=\"profile-detail-stat-grid svelte-1hghrmf\">");
    fmt_num(v, sizeof(v), stat_of(st, "crc")); stat_card(sb, "Crouches", v, 0);
    fmt_num(v, sizeof(v), stat_of(st, "spry")); stat_card(sb, "Sprays", v, 0);
    fmt_num(v, sizeof(v), jnum(d, "jobratingpositive")); stat_card(sb, "KPD Positive", v, 0);
    fmt_num(v, sizeof(v), jnum(d, "jobrating")); stat_card(sb, "KPD Total", v, 0);
    fmt_num(v, sizeof(v), jnum(d, "player_calls")); stat_card(sb, "KPD Calls", v, 0);
    fmt_num(v, sizeof(v), jnum(d, "player_taggedaccounts")); stat_card(sb, "KPD Tagged", v, 0);
    sb_append(sb, "</div> <div class=\"profile-detail-section-header svelte-1hghrmf\">Classes</div> <div class=\"profile-detail-class-grid svelte-1hghrmf\">");
    const ClassConfig *classes = rules_classes();
    const size_t ncls = rules_class_count();
    for (size_t i = 0; classes && i < ncls; i++) {
        if (classes[i].hide || !classes[i].name) continue;
        char key[8], xp[32];
        snprintf(key, sizeof(key), "c%zu", i);
        const double cx = stat_of(st, key);
        fmt_num(xp, sizeof(xp), cx);
        const int clv = (int) floor(0.03 * sqrt(cx)) + 1;
        const Weapon *w = classes[i].loadout ? rules_weapon(classes[i].loadout[0]) : NULL;
        sb_appendf(sb, "<div class=\"profile-detail-class svelte-1hghrmf\"><img class=\"profile-detail-class-icon svelte-1hghrmf\" src=\"./textures/classes/icon_%d.png\" alt=\"%s\"> "
                       "<div class=\"profile-detail-class-info svelte-1hghrmf\"><div class=\"profile-detail-class-name svelte-1hghrmf\">%s</div> "
                       "<div class=\"profile-detail-class-sub svelte-1hghrmf\">Primary: %s</div> <div class=\"profile-detail-class-sub svelte-1hghrmf\">LVL %d · %s XP</div></div></div>",
                   classes[i].icon_index, classes[i].name, classes[i].name, w && w->name ? w->name : "—", clv < 1 ? 1 : clv, xp);
    }
    sb_append(sb, "</div> <div class=\"profile-detail-section-header svelte-1hghrmf\">Raids</div> <div class=\"profile-detail-raid-summary svelte-1hghrmf\">"
                  "<div class=\"profile-detail-raid-sum-grid svelte-1hghrmf\">");
    // raid_stats [{map_id, completions, attempts, best_time (ms), ai_kills, boss_kills, deaths}]: the summary
    // sums them (deob-10.0.3.js:125818-125839), a row per raid follows (126487-126537)
    const cJSON *raids = cJSON_GetObjectItem(d, "raid_stats"), *r;
    double comp = 0, att = 0, aik = 0, bossk = 0, rd = 0;
    cJSON_ArrayForEach(r, raids) {
        comp += jnum(r, "completions");
        att += jnum(r, "attempts");
        aik += jnum(r, "ai_kills");
        bossk += jnum(r, "boss_kills");
        rd += jnum(r, "deaths");
    }
    snprintf(v, sizeof(v), "%.0f/%.0f", comp, att); stat_card(sb, "Completions", v, 0);
    snprintf(v, sizeof(v), "%.0f%%", att > 0 ? floor(comp / att * 100.0 + 0.5) : 0.0); stat_card(sb, "Completion Rate", v, 0); // Math.round
    fmt_num(v, sizeof(v), aik); stat_card(sb, "AI Kills", v, 0);
    fmt_num(v, sizeof(v), bossk); stat_card(sb, "Boss Kills", v, 0);
    fmt_num(v, sizeof(v), rd); stat_card(sb, "Deaths", v, 0);
    sb_append(sb, "</div> ");
    if (cJSON_GetArraySize(raids) == 0) {
        sb_append(sb, "<div class=\"profile-detail-empty svelte-1hghrmf\">No raid data recorded.</div></div>");
        return;
    }
    sb_append(sb, "<div class=\"profile-detail-raid-list svelte-1hghrmf\">");
    cJSON_ArrayForEach(r, raids) {
        const int map = (int) jnum(r, "map_id");
        static const struct { int id; const char *name; } names[] = {{18, "Tortuga"}, {29, "Laboratory"}, {31, "Soul Sanctum"}, {35, "Khepri"}, {43, "Bastion"}};
        const char *rname = NULL;
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) if (names[i].id == map) rname = names[i].name;
        char cleared[48], best[32], k[32], bk[32], dd[32];
        snprintf(cleared, sizeof(cleared), "%.0f/%.0f", jnum(r, "completions"), jnum(r, "attempts"));
        const double bt = jnum(r, "best_time"); // ms: "M:SS", "--:--" when none
        const long long secs = (long long) floor(bt / 1000.0);
        if (bt > 0) snprintf(best, sizeof(best), "%lld:%02lld", secs / 60, secs % 60);
        else snprintf(best, sizeof(best), "--:--");
        fmt_num(k, sizeof(k), jnum(r, "ai_kills"));
        fmt_num(bk, sizeof(bk), jnum(r, "boss_kills"));
        fmt_num(dd, sizeof(dd), jnum(r, "deaths"));
        if (rname) sb_appendf(sb, "<div class=\"profile-detail-raid-row svelte-1hghrmf\"><div class=\"profile-detail-raid-name svelte-1hghrmf\">%s</div> ", rname);
        else sb_appendf(sb, "<div class=\"profile-detail-raid-row svelte-1hghrmf\"><div class=\"profile-detail-raid-name svelte-1hghrmf\">Raid %d</div> ", map);
        sb_append(sb, "<div class=\"profile-detail-raid-grid svelte-1hghrmf\">");
        const char *const labels[5] = {"Cleared", "Best Time", "AI Kills", "Boss Kills", "Deaths"}, *const vals[5] = {cleared, best, k, bk, dd};
        for (int i = 0; i < 5; i++)
            sb_appendf(sb, "<div class=\"profile-detail-raid-cell svelte-1hghrmf\"><span class=\"svelte-1hghrmf\">%s</span> <b class=\"svelte-1hghrmf\">%s</b></div>%s",
                       labels[i], vals[i], i < 4 ? " " : "");
        sb_append(sb, "</div></div>");
    }
    sb_append(sb, "</div></div>");
}

// player_badge / player_titles: "1,5,9", each part parseInt()ed after a trim (deob-10.0.3.js:125762,
// 125763-125770); NaN parts dropped. A number array is read too.
#define PROFILE_IDS_MAX 256
static int profile_ids(const cJSON *v, int *out, const int max) {
    int n = 0;
    const cJSON *e;
    if (cJSON_IsArray(v)) {
        cJSON_ArrayForEach(e, v) if (cJSON_IsNumber(e) && n < max) out[n++] = (int) e->valuedouble;
        return n;
    }
    for (const char *s = cJSON_GetStringValue(v); s && *s && n < max;) {
        char *end;
        const long id = strtol(s, &end, 10); // parseInt: leading blanks skipped, trailing text ignored
        if (end != s) out[n++] = (int) id;
        s = strchr(s, ',');
        if (s) s++;
    }
    return n;
}

static int ids_has(const int *ids, const int n, const int id) {
    for (int i = 0; i < n; i++) if (ids[i] == id) return 1;
    return 0;
}

// the selected badge (deob-10.0.3.js:125736-125745): stats.bdg when the player has that badge, else -1
static int profile_selected_badge(const cJSON *d) {
    const cJSON *b = cJSON_GetObjectItem(cJSON_GetObjectItem(d, "player_stats"), "bdg");
    if (!cJSON_IsNumber(b) || b->valuedouble < 0) return -1;
    int ids[PROFILE_IDS_MAX];
    const int n = profile_ids(cJSON_GetObjectItem(d, "player_badge"), ids, PROFILE_IDS_MAX);
    return ids_has(ids, n, (int) b->valuedouble) && badge_get((int) b->valuedouble) ? (int) b->valuedouble : -1;
}

static void badge_url(const int index, char *out, const size_t n) { // Game.badges[i].url (the asset host)
    char tex[96];
    html_dom_asset_url(badge_texture(index, tex, sizeof(tex)), out, n);
}

// the skins this player made (deob-10.0.3.js:125848-125858): every store skin whose creators list holds
// the lower-cased name, or whose creator text does once lower-cased and split on ", "; store order
static void profile_created_skins(View *v) {
    if (v->cskins_ready || !v->data) return;
    v->cskins_ready = 1;
    char me[64];
    snprintf(me, sizeof(me), "%s", jstr(v->data, "player_name"));
    for (char *c = me; *c; c++) *c = (char) tolower((unsigned char) *c);
    if (!me[0]) return;
    for (int i = 0; i < g_viewer_skin_count; i++) {
        const cJSON *j = ui_loadout_skin_json(i), *c;
        if (!j || !viewer_skin(i)) continue;
        int mine = 0;
        cJSON_ArrayForEach(c, cJSON_GetObjectItem(j, "creators")) if (cJSON_IsString(c) && !strcmp(c->valuestring, me)) mine = 1;
        const char *who = cJSON_GetStringValue(cJSON_GetObjectItem(j, "creator"));
        for (const char *p = who; !mine && p && *p;) {
            const char *q = strstr(p, ", ");
            const size_t len = q ? (size_t) (q - p) : strlen(p);
            mine = len == strlen(me) && !strncasecmp(p, me, len);
            p = q ? q + 2 : NULL;
        }
        if (!mine) continue;
        int *grown = realloc(v->cskins, sizeof(int) * (size_t) (v->ncskins + 1));
        if (!grown) return;
        v->cskins = grown;
        v->cskins[v->ncskins++] = i;
    }
}

// the Badges tab (deob-10.0.3.js:126663-126693, 125771-125774): the player's badges in badge-table order, a tooltip each
static void sb_profile_badges(StrBuf *sb, const cJSON *d) {
    int ids[PROFILE_IDS_MAX];
    const int n = profile_ids(cJSON_GetObjectItem(d, "player_badge"), ids, PROFILE_IDS_MAX);
    sb_append(sb, "<div class=\"profile-detail-badges-panel svelte-1hghrmf\">");
    int shown = 0;
    for (int i = 0; i < BADGE_COUNT; i++) {
        const BadgeDef *b = badge_get(i);
        if (!b || !ids_has(ids, n, i)) continue;
        if (!shown++) sb_append(sb, "<div class=\"profile-detail-badges-grid svelte-1hghrmf\">");
        char url[256];
        badge_url(i, url, sizeof(url));
        sb_append(sb, "<div class=\"profile-detail-badge-item svelte-1hghrmf\" role=\"img\" aria-label=\"");
        sb_append_esc(sb, b->name);
        sb_appendf(sb, "\" onmouseover=\"v10s('btip',%d)\" onmouseout=\"v10s('btipx')\"><img loading=\"lazy\" class=\"svelte-1hghrmf\" src=\"", i);
        sb_append_esc(sb, url);
        sb_append(sb, "\" alt=\"");
        sb_append_esc(sb, b->name);
        sb_append(sb, "\"></div>");
    }
    sb_append(sb, shown ? "</div></div>" : "<div class=\"profile-detail-empty svelte-1hghrmf\">No badges earned.</div></div>");
}

// the Titles tab (deob-10.0.3.js:126694-126721, 125763-125770): the GET /titles rows whose id is in player_titles, in that
// table's order, each in its tag style with its description
static void sb_profile_titles(StrBuf *sb, const cJSON *d) {
    int ids[PROFILE_IDS_MAX];
    const int n = profile_ids(cJSON_GetObjectItem(d, "player_titles"), ids, PROFILE_IDS_MAX);
    sb_append(sb, "<div class=\"profile-detail-badges-panel svelte-1hghrmf\">");
    int shown = 0;
    const cJSON *t, *table = n ? v10_customize_titles() : NULL;
    cJSON_ArrayForEach(t, table) {
        const cJSON *id = cJSON_GetObjectItem(t, "id");
        if (!cJSON_IsNumber(id) || !ids_has(ids, n, id->valueint)) continue;
        if (!shown++) sb_append(sb, "<div class=\"profile-detail-titles-list svelte-1hghrmf\">");
        sb_append(sb, "<div class=\"profile-detail-title-item svelte-1hghrmf\" style=\"");
        StrBuf style = {0};
        v10_customize_title_style(&style, t);
        char *css = sb_finish(&style);
        sb_append_esc(sb, css ? css : "");
        free(css);
        sb_append(sb, "\"><div class=\"profile-detail-title-name svelte-1hghrmf\">");
        sb_append_esc(sb, jstr(t, "name"));
        sb_append(sb, "</div> ");
        if (jstr(t, "desc")[0]) {
            sb_append(sb, "<div class=\"profile-detail-title-desc svelte-1hghrmf\">");
            sb_append_esc(sb, jstr(t, "desc"));
            sb_append(sb, "</div>");
        }
        sb_append(sb, "</div>");
    }
    sb_append(sb, shown ? "</div></div>" : "<div class=\"profile-detail-empty svelte-1hghrmf\">No titles earned.</div></div>");
}

// user-assets thumbnails (v10 _9259 + path): krunker.io's host for a krunker.io account's creations; Box
// Office creations have no thumbnail there (ids differ), so they get v10's fallback picture
static void creation_thumb(StrBuf *sb, const char *path_fmt, const long long id) {
    sb_append(sb, " src=\"");
    if (kio()) {
        char url[160], path[64];
        snprintf(path, sizeof(path), path_fmt, id);
        snprintf(url, sizeof(url), "https://user-assets.krunker.io%s", path);
        sb_append_esc(sb, url);
    } else {
        sb_append(sb, "./img/noimg.png");
    }
    sb_append(sb, "\" onerror=\"this.onerror=null;this.src='./img/noimg.png'\"");
}

// the Creations tab (deob-10.0.3.js:126722-126880): Skins (n) / Games (n) / Mods (n) / Assets (n), on Skins
static void sb_profile_creations(StrBuf *sb, View *v) {
    const cJSON *d = v->data;
    const cJSON *maps = cJSON_GetObjectItem(d, "maps"), *mods = cJSON_GetObjectItem(d, "mods"), *assets = cJSON_GetObjectItem(d, "assets"), *m;
    profile_created_skins(v);
    const int counts[4] = {v->ncskins, cJSON_GetArraySize(maps), cJSON_GetArraySize(mods), cJSON_GetArraySize(assets)};
    static const char *const labels[4] = {"Skins", "Games", "Mods", "Assets"};
    sb_append(sb, "<div class=\"profile-detail-creations-panel svelte-1hghrmf\"><div class=\"simple-tabs svelte-1oqd18e simple-tabs-small\" role=\"tablist\">");
    for (int i = 0; i < 4; i++)
        sb_appendf(sb, "<div class=\"simple-tab svelte-1oqd18e%s\" role=\"tab\" aria-selected=\"%s\" tabindex=\"0\" id=\"v10sCtab%d\" onmouseenter=\"playTick(0.1)\" "
                       "onclick=\"playSelect(0.1);v10s('ctab',%d)\"><span class=\"simple-tab-label svelte-1oqd18e\">%s (%d)</span></div>",
                   v->ctab == i ? " simple-tab-active" : "", v->ctab == i ? "true" : "false", i, i, labels[i], counts[i]);
    sb_append(sb, "</div> ");
    char num[32];
    switch (v->ctab) {
    case 0: // a card per skin, in its rarity's colour; a click opens its item page (the Catalog's detail view)
        if (!v->ncskins) {
            sb_append(sb, "<div class=\"profile-detail-empty svelte-1hghrmf\">No skins created.</div>");
            break;
        }
        sb_append(sb, "<div class=\"profile-detail-creation-grid svelte-1hghrmf\">");
        for (int i = 0; i < v->ncskins; i++) {
            const ViewerSkin *s = viewer_skin(v->cskins[i]);
            const int r = s ? s->rarity : 0;
            const char *col = g_viewer_rarities[r >= 0 && r < g_viewer_rarity_count ? r : 0].color;
            char url[256];
            store_skin_preview(v->cskins[i], url, sizeof(url));
            sb_appendf(sb, "<button class=\"profile-detail-creation-card svelte-1hghrmf\" type=\"button\" style=\"border-color: %s;\" onclick=\"v10s('pskin',%d)\">"
                           "<img loading=\"lazy\" class=\"svelte-1hghrmf\" src=\"", col ? col : "#B0B0B0", v->cskins[i]);
            sb_append_esc(sb, url);
            sb_append(sb, "\" alt=\"");
            sb_append_esc(sb, s && s->name ? s->name : "");
            sb_append(sb, "\"> <div class=\"profile-detail-creation-name svelte-1hghrmf\">");
            sb_append_esc(sb, s && s->name ? s->name : "");
            sb_append(sb, "</div></button>");
        }
        sb_append(sb, "</div>");
        break;
    case 1: // v10 opens the map's page on a click (_32874, not ported)
        if (!counts[1]) {
            sb_append(sb, "<div class=\"profile-detail-empty svelte-1hghrmf\">No maps created.</div>");
            break;
        }
        sb_append(sb, "<div class=\"profile-detail-creation-grid svelte-1hghrmf\">");
        cJSON_ArrayForEach(m, maps) {
            sb_append(sb, "<button class=\"profile-detail-creation-card svelte-1hghrmf\" type=\"button\"><img class=\"profile-detail-creation-thumb svelte-1hghrmf\" loading=\"lazy\"");
            creation_thumb(sb, "/m%lld/thumb.png", (long long) jnum(m, "mid"));
            sb_append(sb, " alt=\"");
            sb_append_esc(sb, jstr(m, "mna"));
            sb_append(sb, "\"> <div class=\"profile-detail-creation-name svelte-1hghrmf\">");
            sb_append_esc(sb, jstr(m, "mna"));
            commas(num, sizeof(num), jnum(m, "vte"));
            sb_appendf(sb, "</div> <div class=\"profile-detail-creation-sub svelte-1hghrmf\"><i class=\"material-icons svelte-1hghrmf\">thumb_up</i> %s</div></button>", num);
        }
        sb_append(sb, "</div>");
        break;
    case 2: // v10 opens the mod's page on a click (_33240, not ported); a featured mod has the gold star
        if (!counts[2]) {
            sb_append(sb, "<div class=\"profile-detail-empty svelte-1hghrmf\">No mods created.</div>");
            break;
        }
        sb_append(sb, "<div class=\"profile-detail-creation-grid svelte-1hghrmf\">");
        cJSON_ArrayForEach(m, mods) {
            sb_append(sb, "<button class=\"profile-detail-creation-card svelte-1hghrmf\" type=\"button\"><div class=\"profile-detail-creation-name svelte-1hghrmf\">");
            sb_append_esc(sb, jstr(m, "mna"));
            commas(num, sizeof(num), jnum(m, "vte"));
            sb_appendf(sb, "</div> <div class=\"profile-detail-creation-sub svelte-1hghrmf\"><i class=\"material-icons svelte-1hghrmf\">thumb_up</i> %s %s</div></button>", num,
                       jtrue(m, "ftr") ? "<i class=\"material-icons svelte-1hghrmf\" style=\"color:#ffd042;\">star</i>" : "");
        }
        sb_append(sb, "</div>");
        break;
    default: { // assets: the texture of a plain texture asset (dat.x, not dat.v), else its thumbnail (dat.t)
        if (!counts[3]) {
            sb_append(sb, "<div class=\"profile-detail-empty svelte-1hghrmf\">No assets uploaded.</div>");
            break;
        }
        sb_append(sb, "<div class=\"profile-detail-creation-grid svelte-1hghrmf\">");
        cJSON_ArrayForEach(m, assets) {
            const cJSON *dv = cJSON_GetObjectItem(m, "dat");
            cJSON *dat = cJSON_IsString(dv) ? cJSON_Parse(dv->valuestring) : NULL;
            const cJSON *meta = dat ? dat : dv;
            const long long id = (long long) jnum(m, "id");
            sb_append(sb, "<div class=\"profile-detail-creation-card svelte-1hghrmf\"><img class=\"profile-detail-creation-thumb svelte-1hghrmf\" loading=\"lazy\"");
            if (jtrue(meta, "x") && !jtrue(meta, "v")) creation_thumb(sb, "/%lld/texture.png", id);
            else if (jtrue(meta, "t")) creation_thumb(sb, "/%lld/thumb.png", id);
            else sb_append(sb, " src=\"./img/noimg.png\"");
            cJSON_Delete(dat);
            sb_append(sb, " alt=\"");
            sb_append_esc(sb, jstr(m, "nm"));
            sb_append(sb, "\"> <div class=\"profile-detail-creation-name svelte-1hghrmf\">");
            sb_append_esc(sb, jstr(m, "nm"));
            sb_append(sb, "</div> ");
            if (jtrue(m, "ini")) {
                char date[32];
                fmt_date(date, sizeof(date), jstr(m, "ini"));
                sb_appendf(sb, "<div class=\"profile-detail-creation-sub svelte-1hghrmf\">%s</div>", date);
            }
            sb_append(sb, "</div>");
        }
        sb_append(sb, "</div>");
        break;
    }
    }
    sb_append(sb, "</div>");
}

static void sb_profile_right(StrBuf *sb, View *v) {
    const cJSON *d = v->data;
    const AccountInfo *a = active_account();
    const int ntabs = a && (a->is_dev || a->is_mod) ? 8 : 7;
    sb_append(sb, "<div class=\"simple-tabs svelte-1oqd18e simple-tabs-small\" role=\"tablist\">");
    for (int i = 0; i < ntabs; i++)
        sb_appendf(sb, "<div class=\"simple-tab svelte-1oqd18e%s\" role=\"tab\" aria-selected=\"%s\" tabindex=\"0\" id=\"v10sPtab%d\" onmouseenter=\"playTick(0.1)\" "
                       "onclick=\"playSelect(0.1);v10s('ptab',%d)\"><span class=\"simple-tab-label svelte-1oqd18e\">%s</span></div>",
                   v->tab == i ? " simple-tab-active" : "", v->tab == i ? "true" : "false", i, i, k_profile_tabs[i]);
    sb_append(sb, "</div> ");
    switch (v->tab) {
    case 0: sb_profile_stats(sb, d); break;
    case 1:
        if (v->feed_loading) sb_append(sb, "<div class=\"profile-detail-loading-inline svelte-1hghrmf\"><i class=\"material-icons spin svelte-1hghrmf\">hourglass_empty</i></div>");
        else if (cJSON_GetArraySize(v->feed) == 0) sb_append(sb, "<div class=\"profile-detail-empty svelte-1hghrmf\">No posts yet.</div>");
        else {
            const cJSON *p;
            cJSON_ArrayForEach(p, v->feed) sb_post(sb, p, 1);
        }
        break;
    case 2: {
        static const char *const ranges[7] = {"1 Week", "2 Weeks", "3 Weeks", "1 Month", "2 Months", "3 Months", "4 Months"};
        double total = 0;
        const cJSON *e;
        cJSON_ArrayForEach(e, v->sales) total += jnum(e, "f");
        char t[32];
        fmt_num(t, sizeof(t), total);
        sb_appendf(sb, "<div class=\"profile-detail-sales-panel svelte-1hghrmf\"><div class=\"profile-detail-sales-header svelte-1hghrmf\"><span>Player Sales</span> "
                       "<span class=\"profile-detail-sales-total svelte-1hghrmf\">Total: %s KR</span></div> <div class=\"profile-detail-sales-ranges svelte-1hghrmf\">", t);
        for (int i = 0; i < 7; i++)
            sb_appendf(sb, "<button class=\"profile-detail-range-btn svelte-1hghrmf%s\" type=\"button\" onclick=\"v10s('range',%d)\">%s</button>",
                       v->sales_range == i ? " active" : "", i, ranges[i]);
        sb_append(sb, "</div> <div class=\"profile-detail-sales-canvas svelte-1hghrmf\">");
        if (v->sales_loading) sb_append(sb, "<div class=\"profile-detail-loading-inline svelte-1hghrmf\"><i class=\"material-icons spin svelte-1hghrmf\">hourglass_empty</i></div>");
        else if (cJSON_GetArraySize(v->sales) == 0) sb_append(sb, "<div class=\"profile-detail-empty svelte-1hghrmf\">No sales in this window.</div>");
        else {
            // a bar per day with sales (v10 draws a line chart on a canvas: _36123)
            double max = 1;
            cJSON_ArrayForEach(e, v->sales) max = fmax(max, jnum(e, "f"));
            sb_append(sb, "<div style=\"display:flex;align-items:flex-end;height:160px;gap:2px\">");
            cJSON_ArrayForEach(e, v->sales) {
                sb_appendf(sb, "<div title=\"%s: %.0f KR\" style=\"flex:1;background:#fbc02d;height:%.0f%%\"></div>", jstr(e, "d"), jnum(e, "f"),
                           jnum(e, "f") / max * 100.0);
            }
            sb_append(sb, "</div>");
        }
        sb_appendf(sb, "</div></div> <div class=\"profile-detail-sales-panel svelte-1hghrmf\"><div class=\"profile-detail-sales-header svelte-1hghrmf\"><span>Active Listings</span> "
                       "<span class=\"profile-detail-sales-total svelte-1hghrmf\">%d live</span></div> ", cJSON_GetArraySize(v->listings));
        if (v->listings_loading) sb_append(sb, "<div class=\"profile-detail-loading-inline svelte-1hghrmf\"><i class=\"material-icons spin svelte-1hghrmf\">hourglass_empty</i></div>");
        else if (cJSON_GetArraySize(v->listings) == 0) sb_append(sb, "<div class=\"profile-detail-empty svelte-1hghrmf\">No active market listings.</div>");
        else {
            sb_append(sb, "<div class=\"profile-detail-listings-grid svelte-1hghrmf\">");
            cJSON_ArrayForEach(e, v->listings) {
                char price[32];
                fmt_num(price, sizeof(price), jnum(e, "kr"));
                sb_appendf(sb, "<button class=\"profile-detail-listing-card svelte-1hghrmf\" type=\"button\"><div class=\"profile-detail-listing-info svelte-1hghrmf\">"
                               "<div class=\"profile-detail-listing-name svelte-1hghrmf\">#%.0f</div> <div class=\"profile-detail-listing-price svelte-1hghrmf\">%s KR</div></div></button>",
                           jnum(e, "sk"), price);
            }
            sb_append(sb, "</div>");
        }
        sb_append(sb, "</div>");
        break;
    }
    case 3: sb_profile_badges(sb, d); break;
    case 4: sb_profile_titles(sb, d); break;
    case 5: sb_profile_creations(sb, v); break;
    case 6: sb_append(sb, "<div class=\"profile-detail-empty svelte-1hghrmf\">No recent matches.</div>"); break;
    case 7: { // Reports (v10 "Reports", developers and moderators): the reports of this player, newest first
        if (v->reports_loading) sb_append(sb, "<div class=\"profile-detail-loading-inline svelte-1hghrmf\"><i class=\"material-icons spin svelte-1hghrmf\">hourglass_empty</i></div>");
        else if (!cJSON_GetArraySize(v->reports)) sb_append(sb, "<div class=\"profile-detail-empty svelte-1hghrmf\">No reports.</div>");
        else {
            const cJSON *rp;
            int row = 0;
            cJSON_ArrayForEach(rp, v->reports) {
                const int idx = (int) jnum(rp, "rp_index");
                sb_appendf(sb, "<div class=\"profile-detail-report svelte-1hghrmf\" id=\"v10sReport%d\"><div class=\"profile-detail-report-head svelte-1hghrmf\"><b>", row++);
                sb_append(sb, idx >= 0 && idx < 7 ? k_report_reasons[idx] : "Unknown");
                sb_append(sb, "</b> <span>from </span>");
                sb_append_esc(sb, jstr(rp, "rp_reportername"));
                sb_append(sb, "</div><div class=\"profile-detail-report-text svelte-1hghrmf\">");
                sb_append_esc(sb, jstr(rp, "rp_text")); // raw from the brain, as v10.0.3's text node (deob-10.0.3.js:126950)
                sb_append(sb, "</div></div>");
            }
        }
        break;
    }
    default: sb_append(sb, "<div class=\"profile-detail-empty svelte-1hghrmf\">No reports.</div>"); break;
    }
}

static void sb_profile(StrBuf *sb, View *v) {
    const char *title = v->data ? jstr(v->data, "player_name") : v->name;
    sb_append(sb, "<div class=\"profile-detail-root svelte-1hghrmf\" role=\"dialog\" aria-label=\"");
    sb_append_esc(sb, title[0] ? title : "Player profile");
    sb_append(sb, "\" id=\"v10sProfile\"><div class=\"profile-detail-header-bar svelte-1hghrmf\"><button class=\"profile-detail-back svelte-1hghrmf\" type=\"button\" aria-label=\"Back\" "
                  "id=\"v10sBack\" onclick=\"v10s('back')\"><i class=\"material-icons svelte-1hghrmf\">arrow_back</i></button> <i class=\"material-icons profile-detail-header-icon svelte-1hghrmf\">person</i> "
                  "<span class=\"profile-detail-header-title svelte-1hghrmf\">");
    sb_append_esc(sb, title);
    sb_append(sb, "</span> ");
    sb_share(sb, "profile", title, 0);
    sb_append(sb, "</div> ");
    if (v->loading) {
        sb_append(sb, "<div class=\"profile-detail-loading svelte-1hghrmf\"><i class=\"material-icons spin svelte-1hghrmf\">hourglass_empty</i></div></div>");
        return;
    }
    if (!v->data) {
        sb_append(sb, "<div class=\"profile-detail-error svelte-1hghrmf\">");
        sb_append_esc(sb, v->error);
        sb_append(sb, "</div></div>");
        return;
    }
    const cJSON *d = v->data;
    const char *name = jstr(d, "player_name"), *clan = jstr(d, "player_clan"), *alias = jstr(d, "player_alias");
    const int online = (int) jnum(d, "online") == 1, premium = jnum(d, "player_premium") > 0;
    sb_appendf(sb, "<div class=\"profile-detail-body svelte-1hghrmf\"><section class=\"profile-detail-identity svelte-1hghrmf\"><div class=\"profile-detail-avatar svelte-1hghrmf %s\">",
               online ? "online" : "offline");
    sb_avatar(sb, name);
    sb_appendf(sb, " <span class=\"profile-detail-online-dot svelte-1hghrmf%s\"></span></div> <div class=\"profile-detail-name-row svelte-1hghrmf\">", online ? " online-on" : "");
    if (jtrue(d, "creator_badge")) sb_append(sb, "<i class=\"material-icons profile-detail-badge-creator svelte-1hghrmf\" title=\"Creator\">verified</i> ");
    if (premium) sb_append(sb, "<i class=\"material-icons profile-detail-badge-premium svelte-1hghrmf\" title=\"Premium\">beenhere</i> ");
    if (jtrue(d, "player_featured")) sb_append(sb, "<i class=\"material-icons profile-detail-badge-featured svelte-1hghrmf\" title=\"Featured\">check_circle</i> ");
    const int rank = (int) jnum(d, "ranked_rank");
    if (rank > 0) // the ranked chip (deob-10.0.3.js:126135-126147): the rank's icon and name in its colour
        sb_appendf(sb, "<span class=\"profile-detail-rank-chip svelte-1hghrmf\" style=\"border-color: %s;\"><img alt=\"Rank\" class=\"profile-detail-rank-icon svelte-1hghrmf\" src=\"./%s\"> "
                       "<span style=\"color: %s;\">%s</span></span> ",
                   ranked_rank_color(rank), ranked_rank_icon(rank), ranked_rank_color(rank), ranked_rank_name(rank));
    const int sel = profile_selected_badge(d);
    if (sel >= 0) { // the selected badge (deob-10.0.3.js:126148-126156)
        char url[256];
        badge_url(sel, url, sizeof(url));
        sb_append(sb, "<img class=\"profile-detail-selected-badge svelte-1hghrmf\" src=\"");
        sb_append_esc(sb, url);
        sb_append(sb, "\" alt=\"");
        sb_append_esc(sb, badge_get(sel)->name);
        sb_append(sb, "\" title=\"");
        sb_append_esc(sb, badge_get(sel)->name);
        sb_append(sb, "\">");
    }
    sb_append(sb, "</div> <div class=\"profile-detail-name svelte-1hghrmf\">");
    sb_append_esc(sb, name);
    sb_append(sb, "</div> ");
    if (premium && alias[0] && strcmp(alias, name)) {
        sb_append(sb, "<div class=\"profile-detail-alias svelte-1hghrmf\">\"");
        sb_append_esc(sb, alias);
        sb_append(sb, "\"</div> ");
    }
    if (clan[0]) {
        sb_append(sb, "<button class=\"profile-detail-clan svelte-1hghrmf\" type=\"button\" id=\"v10sProfileClan\" onclick=\"openClanView(");
        sb_arg(sb, clan);
        sb_append(sb, ")\">[");
        sb_append_esc(sb, clan);
        sb_append(sb, "]</button> ");
    }
    if ((int) jnum(d, "player_hack") == 1) sb_append(sb, "<div class=\"profile-detail-banned svelte-1hghrmf\">BANNED</div> ");
    const double score = jnum(d, "player_score");
    const int lvl = level_of(score);
    const double prev = lvl > 1 ? pow(lvl / 0.03, 2) : 0, need = floor(pow((lvl + 1) / 0.03, 2) - prev), cur = floor(score - prev);
    char curs[32], needs[32], since[32], fol[32], fing[32];
    commas(curs, sizeof(curs), cur);
    commas(needs, sizeof(needs), need);
    fmt_date(since, sizeof(since), jstr(d, "player_datenew"));
    commas(fol, sizeof(fol), jnum(d, "player_followed"));
    commas(fing, sizeof(fing), jnum(d, "player_following"));
    sb_appendf(sb, "<div class=\"profile-detail-xp-strip svelte-1hghrmf\"><div class=\"profile-detail-level-row svelte-1hghrmf\"><span class=\"profile-detail-level-badge svelte-1hghrmf\">LVL %d</span> "
                   "<span class=\"profile-detail-xp-text svelte-1hghrmf\">%s / %s</span></div> <div class=\"profile-detail-xp-bar svelte-1hghrmf\">"
                   "<div class=\"profile-detail-xp-fill svelte-1hghrmf\" style=\"width:%g%%\"></div></div></div> "
                   "<div class=\"profile-detail-info-row svelte-1hghrmf\"><div class=\"profile-detail-label svelte-1hghrmf\">Account Since</div> <div class=\"profile-detail-value svelte-1hghrmf\">%s</div></div> "
                   "<div class=\"profile-detail-social-graph svelte-1hghrmf\"><button class=\"profile-detail-follow-box svelte-1hghrmf\" type=\"button\" onclick=\"v10s('follows',0)\">"
                   "<div class=\"profile-detail-follow-count svelte-1hghrmf\" id=\"v10sFollowers\">%s</div> <div class=\"profile-detail-follow-label svelte-1hghrmf\">Followers</div></button> "
                   "<button class=\"profile-detail-follow-box svelte-1hghrmf\" type=\"button\" onclick=\"v10s('follows',1)\"><div class=\"profile-detail-follow-count svelte-1hghrmf\">%s</div> "
                   "<div class=\"profile-detail-follow-label svelte-1hghrmf\">Following</div></button></div> ",
               lvl, curs, needs, need > 0 ? fmin(100.0, cur / need * 100.0) : 0.0, since, fol, fing);
    if (jtrue(d, "player_twitchname")) { // the Twitch button (deob-10.0.3.js:126203-126210, 125915-125917): opens twitch.tv/<name>
        sb_append(sb, "<button class=\"profile-detail-twitch-btn svelte-1hghrmf\" type=\"button\" id=\"v10sProfileTwitch\" onclick=\"v10s('twitch')\">"
                      "<img src=\"./img/twitch.png\" alt=\"Twitch\" class=\"profile-detail-twitch-img svelte-1hghrmf\"> <span>");
        sb_append_esc(sb, jstr(d, "player_twitchname"));
        sb_append(sb, "</span></button> ");
    }
    sb_append(sb, "<div class=\"profile-detail-actions svelte-1hghrmf\">");
    const AccountInfo *a = actor(); // Follow / Trade / Gift KR / Report
    if (a && strcasecmp(a->name, name) != 0) {
        const int following = jnum(d, "follow_state") > 0;
        sb_appendf(sb, "<button class=\"profile-detail-action svelte-1hghrmf%s\" type=\"button\" id=\"v10sProfileFollow\" onclick=\"v10s('follow')\"><i class=\"material-icons svelte-1hghrmf\">%s</i> <span>%s</span></button> ",
                   following ? " active" : "", following ? "person_remove" : "person_add", following ? "Unfollow" : "Follow");
        if (a->level >= 10 && lvl >= 10)
            sb_append(sb, "<button class=\"profile-detail-action svelte-1hghrmf\" type=\"button\" onclick=\"v10s('trade')\"><i class=\"material-icons svelte-1hghrmf\">swap_horiz</i> <span>Trade</span></button> ");
        sb_append(sb, "<button class=\"profile-detail-action svelte-1hghrmf\" type=\"button\" id=\"v10sProfileGift\" onclick=\"v10s('gift')\"><i class=\"material-icons svelte-1hghrmf\">card_giftcard</i> <span>Gift KR</span></button> "
                      "<button class=\"profile-detail-action svelte-1hghrmf\" type=\"button\" id=\"v10sProfileReport\" onclick=\"v10s('report')\"><i class=\"material-icons svelte-1hghrmf\">report</i> <span>Report</span></button>");
    }
    sb_append(sb, "</div></section> <section class=\"profile-detail-right svelte-1hghrmf\">");
    sb_profile_right(sb, v);
    sb_append(sb, "</section></div></div>");
}

// ---- clan view (_34484)

static void clan_done(const int status, const cJSON *data, const cJSON *error, const intptr_t tag) {
    View *v = view_of(tag);
    if (!v) return;
    v->loading = 0;
    if (!ok(status, error) || !data) snprintf(v->error, sizeof(v->error), "%s", v10_gapi_error_text(error, "Clan not found"));
    else v->data = cJSON_Duplicate(data, 1);
    render_views();
}

static void open_clan(const char *name, const int64_t id) {
    if ((!name || !name[0]) && id <= 0) return;
    View *v = view_push(V_CLAN);
    snprintf(v->name, sizeof(v->name), "%s", name ? name : "");
    v->id = id;
    v->loading = 1;
    char enc[96], path[160];
    if (id > 0) snprintf(path, sizeof(path), "/clans/%lld/profile", (long long) id);
    else {
        backend_url_encode(enc, sizeof(enc), name);
        snprintf(path, sizeof(path), "/clans/by-name/%s", enc);
    }
    if (!gapi("GET", path, NULL, clan_done, view_tag(v)) && !S.scenario) {
        v->loading = 0;
        snprintf(v->error, sizeof(v->error), "Clan not found");
    }
    render_views();
}

static void sb_clan(StrBuf *sb, View *v) {
    const char *name = v->data ? jstr(v->data, "clan_name") : v->name;
    sb_append(sb, "<div class=\"clan-view-root svelte-5j94nj\" role=\"dialog\" aria-label=\"");
    sb_append_esc(sb, name);
    sb_append(sb, "\" id=\"v10sClanView\"><div class=\"clan-view-header-bar svelte-5j94nj\"><button class=\"clan-view-back svelte-5j94nj\" type=\"button\" aria-label=\"Back\" id=\"v10sBack\" onclick=\"v10s('back')\">"
                  "<i class=\"material-icons svelte-5j94nj\">arrow_back</i></button> <i class=\"material-icons clan-view-header-icon svelte-5j94nj\">groups</i> <span class=\"clan-view-header-title svelte-5j94nj\">[");
    sb_append_esc(sb, name);
    sb_append(sb, "]</span> ");
    sb_share(sb, "clan", name, 0);
    sb_append(sb, "</div> ");
    if (v->loading) {
        sb_append(sb, "<div class=\"clan-view-loading svelte-5j94nj\"><i class=\"material-icons spin svelte-5j94nj\">hourglass_empty</i></div></div>");
        return;
    }
    if (!v->data) {
        sb_append(sb, "<div class=\"clan-view-error svelte-5j94nj\">");
        sb_append_esc(sb, v->error);
        sb_append(sb, "</div></div>");
        return;
    }
    const cJSON *d = v->data, *members = cJSON_GetObjectItem(d, "members");
    const char *creator = jstr(d, "creator_name");
    char score[32];
    commas(score, sizeof(score), jnum(d, "clan_score"));
    // no clan logos are hosted: v10's placeholder
    sb_appendf(sb, "<div class=\"clan-view-body svelte-5j94nj\"><section class=\"clan-view-identity svelte-5j94nj\"><div class=\"clan-view-logo-placeholder svelte-5j94nj\"><i class=\"material-icons svelte-5j94nj\">shield</i></div> "
                   "<div class=\"clan-view-name svelte-5j94nj\" style=\"color: %s;\">[", clan_color(name, "#fff"));
    sb_append_esc(sb, name);
    sb_append(sb, "]</div> <div class=\"clan-view-info svelte-5j94nj\"><div class=\"clan-view-info-item svelte-5j94nj\"><span class=\"clan-view-info-label svelte-5j94nj\">Creator</span> "
                  "<button class=\"clan-view-link svelte-5j94nj\" type=\"button\" onclick=\"openPlayerProfile(");
    sb_arg(sb, creator);
    sb_append(sb, ")\">");
    sb_append_esc(sb, creator);
    sb_appendf(sb, "</button></div> <div class=\"clan-view-info-item svelte-5j94nj\"><span class=\"clan-view-info-label svelte-5j94nj\">Score</span> <span class=\"clan-view-info-value svelte-5j94nj\">%s</span></div> "
                   "<div class=\"clan-view-info-item svelte-5j94nj\"><span class=\"clan-view-info-label svelte-5j94nj\">Members</span> <span class=\"clan-view-info-value svelte-5j94nj\">%d</span></div></div> ",
               score, cJSON_GetArraySize(members));
    if (jstr(d, "clan_link4")[0]) {
        sb_append(sb, "<button class=\"clan-view-discord svelte-5j94nj\" type=\"button\" onclick=\"v10s('discord',");
        sb_arg(sb, jstr(d, "clan_link4"));
        sb_append(sb, ")\"><i class=\"material-icons svelte-5j94nj\">forum</i> <span>Discord</span></button> ");
    }
    const AccountInfo *a = actor(); // Request to Join
    if (a && !a->clan[0]) {
        sb_appendf(sb, "<button class=\"clan-view-join svelte-5j94nj\" type=\"button\"%s id=\"v10sClanJoin\" onclick=\"v10s('apply')\"><i class=\"material-icons svelte-5j94nj\">%s</i> <span>%s</span></button> ",
                   v->apply_busy || v->applied ? " disabled" : "", v->applied ? "check" : "group_add",
                   v->apply_busy ? "Sending..." : v->applied ? "Request Sent" : "Request to Join");
        if (v->apply_error[0]) {
            sb_append(sb, "<div class=\"clan-view-apply-error svelte-5j94nj\">");
            sb_append_esc(sb, v->apply_error);
            sb_append(sb, "</div>");
        }
    }
    sb_append(sb, "</section> <section class=\"clan-view-members svelte-5j94nj\">");
    static const char *const roles[4] = {"Recruit", "Soldier", "Captain", "Commander"};
    for (int r = 3; r >= 0; r--) {
        const cJSON *list[256];
        int n = 0;
        const cJSON *m;
        cJSON_ArrayForEach(m, members) {
            const int is_creator = !strcmp(jstr(m, "p"), creator);
            if (n < 256 && (r == 3 ? is_creator : ((int) jnum(m, "r") == r && !is_creator))) list[n++] = m;
        }
        if (!n) continue;
        for (int i = 1; i < n; i++) { // s7 descending (stable)
            const cJSON *x = list[i];
            int j = i - 1;
            while (j >= 0 && jnum(list[j], "s7") < jnum(x, "s7")) {
                list[j + 1] = list[j];
                j--;
            }
            list[j + 1] = x;
        }
        sb_appendf(sb, "<div class=\"clan-view-role-group svelte-5j94nj\"><div class=\"clan-view-role-header svelte-5j94nj\">%s <span class=\"clan-view-role-count svelte-5j94nj\">(%d)</span></div> ",
                   roles[r], n);
        for (int i = 0; i < n; i++) {
            char abbr[32];
            fmt_abbr(abbr, sizeof(abbr), jnum(list[i], "s7"));
            sb_append(sb, "<button class=\"clan-view-member svelte-5j94nj\" type=\"button\" onclick=\"openPlayerProfile(");
            sb_arg(sb, jstr(list[i], "p"));
            sb_append(sb, ")\"><div class=\"clan-view-member-left svelte-5j94nj\">");
            if (jtrue(list[i], "su")) sb_append(sb, "<i class=\"material-icons clan-view-member-premium svelte-5j94nj\" title=\"Premium\">beenhere</i> ");
            if (jtrue(list[i], "ve")) sb_append(sb, "<i class=\"material-icons clan-view-member-verified svelte-5j94nj\" title=\"Verified\">check_circle</i> ");
            sb_appendf(sb, "<span class=\"clan-view-member-name svelte-5j94nj%s\">", jtrue(list[i], "h") ? " hacked" : "");
            sb_append_esc(sb, jstr(list[i], "p"));
            sb_appendf(sb, "</span></div> <span class=\"clan-view-member-score svelte-5j94nj\">%s</span></button>", abbr);
        }
        sb_append(sb, "</div>");
    }
    sb_append(sb, "</section></div></div>");
}

// ---- post detail (_35483)

static void post_detail_done(const int status, const cJSON *data, const cJSON *error, const intptr_t tag) {
    View *v = view_of(tag);
    if (!v) return;
    v->loading = 0;
    if (!ok(status, error)) snprintf(v->error, sizeof(v->error), "%s", v10_gapi_error_text(error, "Failed to load post"));
    else if (!data) snprintf(v->error, sizeof(v->error), "Post not found");
    else {
        cJSON_Delete(v->data);
        v->data = cJSON_Duplicate(data, 1);
    }
    render_views();
}

static void post_reload(View *v) {
    char path[64];
    snprintf(path, sizeof(path), "/social/post/%lld", (long long) v->id);
    if (!gapi("GET", path, NULL, post_detail_done, view_tag(v)) && !S.scenario) {
        v->loading = 0;
        snprintf(v->error, sizeof(v->error), "Failed to load post");
    }
}

static void open_post(const int64_t id) {
    if (id <= 0) return;
    View *v = view_push(V_POST);
    v->id = id;
    v->loading = 1;
    post_reload(v);
    render_views();
}

static void sb_comment(StrBuf *sb, const cJSON *c) {
    const char *pln = jstr(c, "pln"), *cln = jstr(c, "cln");
    char time_s[64], dur[48];
    if (jnum(c, "tim") > 0) {
        fmt_dur(dur, sizeof(dur), jnum(c, "tim"));
        snprintf(time_s, sizeof(time_s), "%s ago", dur);
    } else snprintf(time_s, sizeof(time_s), "Just now");
    sb_append(sb, "<div class=\"comment-card svelte-7miv1g\"><img class=\"comment-pfp svelte-7miv1g\" alt=\"\" src=\"./img/player_pfp.png\"> <div class=\"comment-body svelte-7miv1g\">"
                  "<div class=\"comment-head svelte-7miv1g\"><button class=\"comment-name svelte-7miv1g\" type=\"button\" onclick=\"openPlayerProfile(");
    sb_arg(sb, pln);
    sb_append(sb, ")\">");
    sb_append_esc(sb, pln);
    sb_append(sb, "</button> ");
    if (cln[0]) {
        sb_appendf(sb, "<button class=\"comment-clan svelte-7miv1g\" type=\"button\" style=\"color: %s\" onclick=\"openClanView(", clan_color(cln, "rgba(255,255,255,0.5)"));
        sb_arg(sb, cln);
        sb_append(sb, ")\">[");
        sb_append_esc(sb, cln);
        sb_append(sb, "]</button> ");
    }
    if (jnum(c, "prm") > 0) sb_append(sb, "<i class=\"material-icons comment-badge svelte-7miv1g\" style=\"color: #fbc02d\">beenhere</i> ");
    if (jnum(c, "ftr") > 0) sb_append(sb, "<i class=\"material-icons comment-badge svelte-7miv1g\" style=\"color: #4fc3f7\">check_circle</i> ");
    sb_appendf(sb, "<span class=\"comment-time svelte-7miv1g\">%s</span></div> ", time_s);
    if (jstr(c, "txt")[0]) {
        sb_append(sb, "<div class=\"comment-text svelte-7miv1g\">");
        sb_append_esc(sb, jstr(c, "txt"));
        sb_append(sb, "</div>");
    }
    sb_append(sb, "</div></div>");
}

static char g_reply[1024];

static void sb_post_detail(StrBuf *sb, View *v) {
    sb_append(sb, "<div class=\"detail-overlay svelte-h3kymj\" role=\"presentation\" onclick=\"v10s('backdrop')\"><div class=\"detail-window svelte-h3kymj\" role=\"dialog\" aria-modal=\"true\" "
                  "aria-label=\"Post detail\" id=\"v10sPostDetail\" onclick=\"v10s('stop')\"><div class=\"detail-head svelte-h3kymj\"><button class=\"detail-back svelte-h3kymj\" type=\"button\" "
                  "aria-label=\"Back to feed\" id=\"v10sBack\" onclick=\"v10s('back')\"><i class=\"material-icons detail-back-icon svelte-h3kymj\">arrow_back</i></button> "
                  "<span class=\"detail-title svelte-h3kymj\">Post</span></div> <div class=\"detail-body svelte-h3kymj\">");
    const cJSON *post = cJSON_GetObjectItem(v->data, "post");
    if (v->loading && !post) sb_append(sb, "<div class=\"detail-status svelte-h3kymj\">Loading…</div>");
    else if (v->error[0] && !post) {
        sb_append(sb, "<div class=\"detail-status detail-error svelte-h3kymj\">");
        sb_append_esc(sb, v->error);
        sb_append(sb, "</div>");
    } else if (post) {
        sb_post(sb, post, 0);
        sb_append(sb, " ");
        if (actor()) { // the reply form
            const char *t = g_reply;
            while (*t == ' ') t++;
            sb_append(sb, "<div class=\"reply-form svelte-h3kymj\"><input class=\"reply-input svelte-h3kymj\" type=\"text\" placeholder=\"Write a reply...\" maxlength=\"500\" id=\"v10sReply\" "
                          "oninput=\"v10sInput('reply',this.value)\" onkeydown=\"v10sKey('reply')\" value=\"");
            sb_append_esc(sb, g_reply);
            sb_appendf(sb, "\"> <button class=\"reply-btn svelte-h3kymj\" type=\"button\"%s id=\"v10sReplyBtn\" onclick=\"v10s('reply')\">%s</button></div> ",
                       !*t || v->sending ? " disabled" : "", v->sending ? "..." : "Reply");
        }
        if (v->reply_error[0]) {
            sb_append(sb, "<div class=\"reply-error svelte-h3kymj\">");
            sb_append_esc(sb, v->reply_error);
            sb_append(sb, "</div> ");
        }
        sb_append(sb, "<div class=\"comment-list svelte-h3kymj\">");
        const cJSON *comments = cJSON_GetObjectItem(v->data, "comments"), *c;
        if (cJSON_GetArraySize(comments) == 0) sb_append(sb, "<div class=\"detail-status svelte-h3kymj\">No comments yet.</div>");
        else cJSON_ArrayForEach(c, comments) sb_comment(sb, c);
        sb_append(sb, "</div>");
    }
    sb_append(sb, "</div></div></div>");
}

// ---- compose (_49624)

static char g_compose_text[2048], g_giveaway_amount[32];

static void sb_compose(StrBuf *sb, View *v) {
    const double amt = atof(g_giveaway_amount);
    const long long fee = (long long) ceil(0.1 * amt), total = (long long) amt + fee;
    const char *t = g_compose_text;
    while (*t == ' ' || *t == '\n') t++;
    const int can = !v->submitting && *t && !(v->giveaway && amt < 10);
    sb_append(sb, "<div class=\"compose-overlay svelte-qxxay9\" role=\"presentation\" onclick=\"v10s('backdrop')\"><div class=\"compose-window svelte-qxxay9\" role=\"dialog\" aria-modal=\"true\" "
                  "aria-label=\"New post\" id=\"v10sComposeWin\" onclick=\"v10s('stop')\"><div class=\"compose-title svelte-qxxay9\">New post</div> <textarea class=\"compose-input svelte-qxxay9\" "
                  "placeholder=\"Say something...\" rows=\"4\" maxlength=\"500\" id=\"v10sComposeText\" oninput=\"v10sInput('compose',this.value)\">");
    sb_append_esc(sb, g_compose_text);
    sb_appendf(sb, "</textarea> <div class=\"compose-toggle svelte-qxxay9\"><label class=\"compose-toggle-label svelte-qxxay9\"><input type=\"checkbox\" id=\"v10sGiveaway\"%s "
                   "onchange=\"v10s('giveaway')\"> <span class=\"compose-toggle-text svelte-qxxay9\">Make this a KR giveaway</span></label></div> ",
               v->giveaway ? " checked" : "");
    if (v->giveaway)
        sb_appendf(sb, "<div class=\"compose-giveaway svelte-qxxay9\"><label class=\"compose-giveaway-label svelte-qxxay9\" for=\"giveaway-kr\">KR amount</label> "
                       "<input id=\"giveaway-kr\" class=\"compose-input-num svelte-qxxay9\" type=\"number\" placeholder=\"0\" min=\"10\" value=\"%s\" oninput=\"v10sInput('amount',this.value)\"> "
                       "<div class=\"compose-giveaway-hint svelte-qxxay9\">Minimum 10 KR.</div> <div class=\"compose-fee svelte-qxxay9\">A 10% fee applies. You will be charged "
                       "<strong class=\"compose-fee-total svelte-qxxay9\">%lld</strong> KR total.</div></div> ",
                   g_giveaway_amount, total);
    if (v->compose_error[0]) {
        sb_append(sb, "<div class=\"compose-error svelte-qxxay9\">");
        sb_append_esc(sb, v->compose_error);
        sb_append(sb, "</div> ");
    }
    sb_appendf(sb, "<div class=\"compose-actions svelte-qxxay9\"><button class=\"compose-btn compose-btn-cancel svelte-qxxay9\" type=\"button\"%s onclick=\"v10s('back')\">Cancel</button> "
                   "<button class=\"compose-btn compose-btn-submit svelte-qxxay9\" type=\"button\"%s id=\"v10sComposePost\" onclick=\"v10s('submit')\">%s</button></div></div></div>",
               v->submitting ? " disabled" : "", can ? "" : " disabled", v->submitting ? "Posting…" : "Post");
}

// ---- follow list (_34637)

static void follows_done(const int status, const cJSON *data, const cJSON *error, const intptr_t tag) {
    View *v = view_of(tag & ~(intptr_t) 0x10000);
    if (!v) return;
    cJSON **slot = tag & 0x10000 ? &v->following : &v->followers;
    cJSON_Delete(*slot);
    *slot = ok(status, error) && cJSON_IsArray(data) ? cJSON_Duplicate(data, 1) : cJSON_CreateArray();
    if (!ok(status, error)) snprintf(v->error, sizeof(v->error), "%s", v10_gapi_error_text(error, "Failed to load"));
    render_views();
}

static void open_follows(const char *name, const int tab, const double followers, const double following) {
    View *v = view_push(V_FOLLOWS);
    snprintf(v->name, sizeof(v->name), "%s", name);
    v->follows_tab = tab;
    v->data = cJSON_CreateObject();
    cJSON_AddNumberToObject(v->data, "followers", followers);
    cJSON_AddNumberToObject(v->data, "following", following);
    char enc[160], path[200];
    backend_url_encode(enc, sizeof(enc), name);
    snprintf(path, sizeof(path), "/players/%s/followers", enc);
    gapi("GET", path, NULL, follows_done, view_tag(v));
    snprintf(path, sizeof(path), "/players/%s/following", enc);
    gapi("GET", path, NULL, follows_done, view_tag(v) | 0x10000);
    render_views();
}

static void sb_follows(StrBuf *sb, View *v) {
    sb_append(sb, "<div class=\"follow-root svelte-1ef5zjv\" role=\"dialog\" aria-label=\"Follow list\"><div class=\"follow-header svelte-1ef5zjv\"><button class=\"follow-back svelte-1ef5zjv\" type=\"button\" "
                  "aria-label=\"Back\" id=\"v10sBack\" onclick=\"v10s('back')\"><i class=\"material-icons svelte-1ef5zjv\">arrow_back</i></button> <i class=\"material-icons follow-header-icon svelte-1ef5zjv\">group</i> "
                  "<span class=\"follow-header-title svelte-1ef5zjv\">");
    sb_append_esc(sb, v->name);
    sb_appendf(sb, "</span></div> <div class=\"follow-tabs svelte-1ef5zjv\"><button class=\"follow-tab svelte-1ef5zjv%s\" type=\"button\" onclick=\"v10s('ftab',0)\">Followers (%.0f)</button> "
                   "<button class=\"follow-tab svelte-1ef5zjv%s\" type=\"button\" onclick=\"v10s('ftab',1)\">Following (%.0f)</button></div> <div class=\"follow-body svelte-1ef5zjv\">",
               v->follows_tab == 0 ? " active" : "", jnum(v->data, "followers"), v->follows_tab == 1 ? " active" : "", jnum(v->data, "following"));
    const cJSON *list = v->follows_tab ? v->following : v->followers;
    if (!list) sb_append(sb, "<div class=\"follow-state-msg svelte-1ef5zjv\"><i class=\"material-icons spin svelte-1ef5zjv\">hourglass_empty</i></div>");
    else if (cJSON_GetArraySize(list) == 0)
        sb_appendf(sb, "<div class=\"follow-state-msg follow-empty svelte-1ef5zjv\">%s</div>", v->follows_tab ? "Not following anyone yet." : "No followers yet.");
    else {
        const cJSON *r;
        const int64_t me = actor() ? my_id() : 0; // the rows' Follow buttons
        cJSON_ArrayForEach(r, list) {
            char since[32];
            fmt_date(since, sizeof(since), jstr(r, "date"));
            sb_append(sb, "<div class=\"follow-row svelte-1ef5zjv\"><button class=\"follow-row-main svelte-1ef5zjv\" type=\"button\" onclick=\"openPlayerProfile(");
            sb_arg(sb, jstr(r, "player_name"));
            sb_append(sb, ")\"><div class=\"follow-avatar svelte-1ef5zjv\">");
            sb_avatar(sb, jstr(r, "player_name"));
            sb_append(sb, "</div> <div class=\"follow-info svelte-1ef5zjv\"><div class=\"follow-name-line svelte-1ef5zjv\">");
            if (jnum(r, "premium") > 0) sb_append(sb, "<i class=\"material-icons follow-badge follow-premium svelte-1ef5zjv\" title=\"Premium\">beenhere</i> ");
            if (jtrue(r, "player_featured")) sb_append(sb, "<i class=\"material-icons follow-badge follow-featured svelte-1ef5zjv\" title=\"Featured\">check_circle</i> ");
            sb_append(sb, "<span class=\"follow-name svelte-1ef5zjv\">");
            sb_append_esc(sb, jstr(r, "player_name"));
            sb_appendf(sb, "</span></div> <div class=\"follow-since svelte-1ef5zjv\">Since %s</div></div></button> ", since);
            if (me && (int64_t) jnum(r, "player_id") != me) {
                const int f = jnum(r, "follow_state") > 0;
                sb_appendf(sb, "<button class=\"follow-btn svelte-1ef5zjv%s\" type=\"button\" onclick=\"v10s('frow',", f ? " following" : "");
                sb_arg(sb, jstr(r, "player_name"));
                sb_appendf(sb, ")\">%s</button>", f ? "Unfollow" : "Follow");
            }
            sb_append(sb, "</div>");
        }
    }
    sb_append(sb, "</div></div>");
}

// ---- gift (_36860) / report (_36916): the economy's "kr" and the hub's "rpt" frames

static char g_gift_amount[32] = "100", g_gift_msg[600], g_report_text[600];
static int g_report_reason;

static void sb_gift(StrBuf *sb, View *v) {
    sb_append(sb, "<div class=\"gift-root svelte-g5j1wf\" role=\"dialog\" aria-label=\"Gift KR\" id=\"v10sGiftWin\"><div class=\"gift-header svelte-g5j1wf\"><button class=\"gift-back svelte-g5j1wf\" type=\"button\" "
                  "aria-label=\"Back\" id=\"v10sBack\" onclick=\"v10s('back')\"><i class=\"material-icons svelte-g5j1wf\">arrow_back</i></button> <i class=\"material-icons gift-header-icon svelte-g5j1wf\">card_giftcard</i> "
                  "<span class=\"gift-header-title svelte-g5j1wf\">Gift KR to ");
    sb_append_esc(sb, v->name);
    sb_append(sb, "</span></div> ");
    if (v->done) {
        sb_append(sb, "<div class=\"gift-success svelte-g5j1wf\"><i class=\"material-icons svelte-g5j1wf\">check_circle</i> <span>");
        sb_append_esc(sb, v->message);
        sb_append(sb, "</span> <button class=\"gift-done svelte-g5j1wf\" type=\"button\" onclick=\"v10s('back')\">Done</button></div></div>");
        return;
    }
    const long long amt = atoll(g_gift_amount), fee = (amt * 10 + 99) / 100;
    sb_appendf(sb, "<div class=\"gift-body svelte-g5j1wf\"><label class=\"gift-field svelte-g5j1wf\"><span class=\"gift-label svelte-g5j1wf\">Amount (10%% fee)</span> "
                   "<input type=\"number\" min=\"10\" class=\"gift-input svelte-g5j1wf\" id=\"v10sGiftAmount\" value=\"%s\" oninput=\"v10sInput('giftamt',this.value)\"></label> "
                   "<label class=\"gift-field svelte-g5j1wf\"><span class=\"gift-label svelte-g5j1wf\">Message (optional)</span> <textarea maxlength=\"500\" rows=\"3\" class=\"gift-input svelte-g5j1wf\" "
                   "id=\"v10sGiftMsg\" oninput=\"v10sInput('giftmsg',this.value)\">",
               g_gift_amount);
    sb_append_esc(sb, g_gift_msg);
    sb_appendf(sb, "</textarea></label> <div class=\"gift-fee svelte-g5j1wf\">Fee: %lld KR (10%%) — Total cost: %lld KR</div> ", fee, amt + fee);
    if (v->error[0]) {
        sb_append(sb, "<div class=\"gift-error svelte-g5j1wf\">");
        sb_append_esc(sb, v->error);
        sb_append(sb, "</div> ");
    }
    if (v->loading) sb_append(sb, "<button class=\"gift-send svelte-g5j1wf\" type=\"button\" disabled>Sending...</button></div></div>");
    else sb_appendf(sb, "<button class=\"gift-send svelte-g5j1wf\" type=\"button\" id=\"v10sGiftSend\" onclick=\"v10s('giftsend')\">Send %lld KR (+%lld fee)</button></div></div>", amt, fee);
}

static void sb_report(StrBuf *sb, View *v) {
    sb_append(sb, "<div class=\"report-root svelte-18p16wl\" role=\"dialog\" aria-label=\"Report player\"><div class=\"report-header svelte-18p16wl\"><button class=\"report-back svelte-18p16wl\" type=\"button\" "
                  "aria-label=\"Back\" id=\"v10sBack\" onclick=\"v10s('back')\"><i class=\"material-icons svelte-18p16wl\">arrow_back</i></button> <i class=\"material-icons report-header-icon svelte-18p16wl\">report</i> "
                  "<span class=\"report-header-title svelte-18p16wl\">Report ");
    sb_append_esc(sb, v->name);
    sb_append(sb, "</span></div> ");
    if (v->done) {
        sb_append(sb, "<div class=\"report-success svelte-18p16wl\"><i class=\"material-icons svelte-18p16wl\">check_circle</i> <span>Report submitted. Thanks for keeping Krunker fair.</span> "
                      "<button class=\"report-done svelte-18p16wl\" type=\"button\" onclick=\"v10s('back')\">Done</button></div></div>");
        return;
    }
    sb_append(sb, "<div class=\"report-body svelte-18p16wl\"><label class=\"report-field svelte-18p16wl\"><span class=\"report-label svelte-18p16wl\">Reason</span> "
                  "<select class=\"report-input svelte-18p16wl\" id=\"v10sReportReason\" onchange=\"v10sInput('reason',this.value)\">");
    for (int i = 0; i < 7; i++) sb_appendf(sb, "<option value=\"%d\"%s>%s</option>", i, g_report_reason == i ? " selected" : "", k_report_reasons[i]);
    sb_append(sb, "</select></label> <label class=\"report-field svelte-18p16wl\"><span class=\"report-label svelte-18p16wl\">Details (min 10 characters)</span> "
                  "<textarea maxlength=\"500\" rows=\"5\" class=\"report-input svelte-18p16wl\" placeholder=\"Provide more information (e.g. links to a video clip)\" id=\"v10sReportText\" "
                  "oninput=\"v10sInput('reporttext',this.value)\">");
    sb_append_esc(sb, g_report_text);
    sb_append(sb, "</textarea></label> ");
    if (v->error[0]) {
        sb_append(sb, "<div class=\"report-error svelte-18p16wl\">");
        sb_append_esc(sb, v->error);
        sb_append(sb, "</div> ");
    }
    sb_appendf(sb, "<button class=\"report-send svelte-18p16wl\" type=\"button\" id=\"v10sReportSend\"%s onclick=\"v10s('reportsend')\">%s</button></div></div>",
               v->loading ? " disabled" : "", v->loading ? "Submitting..." : "Submit Report");
}

// ------------------------------------------------------------------ render: the hub (#genericPop)

static int g_last_bottom_local; // the newest bubble when the conversation was last scrolled down

// re-rendering replaces the markup: keep the focused field (and its caret) and the scrolled lists
typedef struct {
    char focus[48];
    int caret0, caret1, has_caret;
    struct {
        const char *id;
        HtmlScroll scroll;
        int ok;
    } scrolls[12];
    int nscroll;
} Keep;

static void keep_save(Keep *k, const char *const *ids, const int n) {
    memset(k, 0, sizeof(*k));
    const HtmlElem a = html_dom_active_element();
    if (a && html_dom_get_attr(a, "id", k->focus, sizeof(k->focus)) >= 0 &&
        (!strncmp(k->focus, "v10s", 4) || !strcmp(k->focus, "giveaway-kr")))
        k->has_caret = html_dom_text_selection(a, &k->caret0, &k->caret1);
    else k->focus[0] = 0;
    for (int i = 0; i < n && k->nscroll < 12; i++) {
        const HtmlElem e = html_dom_by_id(ids[i]);
        k->scrolls[k->nscroll].id = ids[i];
        k->scrolls[k->nscroll].ok = e && html_dom_get_scroll(e, &k->scrolls[k->nscroll].scroll);
        k->nscroll++;
    }
}

static void keep_restore(const Keep *k) {
    for (int i = 0; i < k->nscroll; i++) {
        const HtmlElem e = k->scrolls[i].ok && k->scrolls[i].scroll.top > 0 ? html_dom_by_id(k->scrolls[i].id) : 0;
        HtmlScroll now;
        // the new markup's layout first (get_scroll flushes it), so the restore clamps to it
        if (e && html_dom_get_scroll(e, &now)) html_dom_set_scroll(e, k->scrolls[i].scroll.left, k->scrolls[i].scroll.top);
    }
    if (k->focus[0]) {
        const HtmlElem e = html_dom_by_id(k->focus);
        if (e) {
            html_dom_focus(e);
            if (k->has_caret) html_dom_text_set_selection(e, k->caret0, k->caret1);
        }
    }
}

static int hub_shown(void) {
    const HtmlElem pop = html_dom_by_id("genericPop");
    char cls[64] = "";
    return pop && html_dom_is_shown(pop) && html_dom_get_attr(pop, "class", cls, sizeof(cls)) >= 0 && !strcmp(cls, "socialModal");
}

// KRUNKNATIVE_V10_SOCIAL_DUMP=<file.json> (--render-scenario): the hub / view-stack markup and the
// page scale, for tools/uidiff/v10_social_ref.py (the Chromium reference of the same markup)
static char *g_dump_hub, *g_dump_views;
static void dump_state(void) {
    const char *path = S.scenario ? getenv("KRUNKNATIVE_V10_SOCIAL_DUMP") : NULL;
    if (!path || !path[0]) return;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "hub", g_dump_hub ? g_dump_hub : "");
    cJSON_AddStringToObject(o, "views", g_dump_views ? g_dump_views : "");
    static const char *const props[] = {"transform", "width", "height"};
    const HtmlElem base = html_dom_by_id("uiBase");
    cJSON *b = cJSON_AddObjectToObject(o, "uiBase");
    for (int i = 0; i < 3; i++) {
        char v[64] = "";
        if (base && html_dom_get_style(base, props[i], v, sizeof(v)) >= 0) cJSON_AddStringToObject(b, props[i], v);
    }
    static const char *const vars[] = {"--ui-scale", "--ui-width", "--ui-height"};
    cJSON *bd = cJSON_AddObjectToObject(o, "body");
    for (int i = 0; i < 3; i++) {
        char v[64] = "";
        if (html_dom_get_style(html_dom_body(), vars[i], v, sizeof(v)) >= 0) cJSON_AddStringToObject(bd, vars[i], v);
    }
    char *text = cJSON_Print(o);
    FILE *f = fopen(path, "w");
    if (f && text) fputs(text, f);
    if (f) fclose(f);
    free(text);
    cJSON_Delete(o);
}

static void render(void) {
    if (!S.open) return;
    const HtmlElem pop = html_dom_by_id("genericPop");
    if (!pop) return;
    StrBuf sb = {0};
    static const char *const tabs[3][2] = {{"public", "Social"}, {"people", "Friends"}, {"search", "Search"}};
    const int unread = v10_social_unread_total();
    sb_append(&sb, "<div class=\"social-root svelte-xp7z8d\"><div class=\"social-sidebar svelte-xp7z8d\">");
    for (int i = 0; i < 3; i++) {
        sb_appendf(&sb, "<div class=\"sidebar-tab svelte-berov0%s\" role=\"tab\" tabindex=\"0\" aria-selected=\"%s\" id=\"v10sTab%d\" onmouseenter=\"playTick(0.1)\" "
                        "onclick=\"playSelect(0.1);v10s('tab',%d)\"><i class=\"material-icons sidebar-tab-icon svelte-berov0\">%s</i> <span class=\"sidebar-tab-label svelte-berov0\">%s</span> ",
                   S.tab == i ? " active" : "", S.tab == i ? "true" : "false", i, i, tabs[i][0], tabs[i][1]);
        if (i == TAB_FRIENDS && unread > 0 && S.tab != i) {
            if (unread > 99) sb_appendf(&sb, "<span class=\"sidebar-tab-badge svelte-berov0\" aria-label=\"%d unread\">99+</span>", unread);
            else sb_appendf(&sb, "<span class=\"sidebar-tab-badge svelte-berov0\" aria-label=\"%d unread\">%d</span>", unread, unread);
        }
        sb_append(&sb, "</div>");
    }
    sb_append(&sb, "</div> <div class=\"social-content svelte-xp7z8d\">");
    if (S.tab == TAB_SOCIAL) sb_social_tab(&sb);
    else if (S.tab == TAB_FRIENDS) sb_friends_tab(&sb);
    else sb_search_tab(&sb);
    sb_append(&sb, "</div></div>");
    char *html = sb_finish(&sb);
    static const char *const scroll_ids[] = {"v10sFeed0", "v10sFeed1", "v10sFeed2", "v10sFeed3", "v10sFeed4", "v10sFriendScroll", "v10sMessages", "v10sSearchContent"};
    Keep k;
    keep_save(&k, scroll_ids, 8);
    html_dom_set_html(pop, html);
    if (S.scenario) {
        free(g_dump_hub);
        g_dump_hub = html;
        dump_state();
    } else {
        free(html);
    }
    keep_restore(&k);
    // the conversation follows its newest message
    const Conv *c = S.tab == TAB_FRIENDS && S.selected ? conv_get(S.selected, 0) : NULL;
    if (c && !c->loading && S.earlier_anchor > 0.0f) { // an earlier page landed: the same distance from the bottom
        const HtmlElem m = html_dom_by_id("v10sMessages");
        HtmlScroll sc;
        if (m && html_dom_get_scroll(m, &sc)) html_dom_set_scroll(m, sc.left, sc.height - S.earlier_anchor);
        S.earlier_anchor = 0.0f;
        if (m && html_dom_get_scroll(m, &sc)) S.msgs_top = sc.top;
    }
    const int last = c && c->n ? c->msgs[c->n - 1].local_id : 0;
    if (c && last != g_last_bottom_local) {
        const HtmlElem m = html_dom_by_id("v10sMessages");
        HtmlScroll sc;
        if (m && html_dom_get_scroll(m, &sc)) html_dom_set_scroll(m, sc.left, sc.height > sc.client_h ? sc.height - sc.client_h : 0);
        g_last_bottom_local = last;
    }
    S.rendered = 1;
}

// v10 mounts a tab's component afresh whenever it is shown (openSocial builds a new panel, a tab switch a new
// pane): the Social tab starts on Global with an empty search (deob-10.0.3.js:157431, 157434), the Friends tab
// with no friend selected, no filter and an empty draft (157120, 157633, 157697). The Search tab keeps the
// panel's query (its initialQuery, 158253), which a new panel starts empty.
static void mount_tab(const int tab) {
    if (tab == TAB_SOCIAL) {
        S.sub = 0;
        S.social_query[0] = 0;
    } else if (tab == TAB_FRIENDS) {
        S.selected = 0;
        S.friend_filter[0] = 0;
        S.compose[0] = 0;
    }
}

// genericPopup({component: _49999}, "socialModal"), with the state as it is (the scenario renderer's presets)
static void show_hub(const int tab) {
    ui_loadout_clear_pops(); // resetPops()
    const HtmlElem holder = html_dom_by_id("popupHolder"), pop = html_dom_by_id("genericPop");
    if (!pop) return;
    if (holder) html_dom_set_display(holder, "block");
    html_dom_set_attr(pop, "class", "socialModal");
    html_dom_set_attr(pop, "style", "");
    html_dom_set_display(pop, "block");
    S.open = 1;
    S.tab = tab >= 0 && tab <= 2 ? tab : 0;
    if (S.tab == TAB_SOCIAL) feeds_load(1);
    g_last_bottom_local = 0;
    render();
}

// openSocial(initialTab): a new panel, no initialQuery (deob-10.0.3.js:252405-252411)
static void open_hub(const int tab) {
    S.search_q[0] = 0;
    search_changed();
    mount_tab(tab >= 0 && tab <= 2 ? tab : 0);
    show_hub(tab);
}

// ------------------------------------------------------------------ render: the view stack (#app's .view-stack)

static void render_views(void) {
    View *v = top_view();
    if (!v || v->depth != v10_view_depth()) return; // another module's view is on top
    StrBuf sb = {0};
    switch (v->type) {
    case V_PROFILE: sb_profile(&sb, v); break;
    case V_CLAN: sb_clan(&sb, v); break;
    case V_POST: sb_post_detail(&sb, v); break;
    case V_COMPOSE: sb_compose(&sb, v); break;
    case V_FOLLOWS: sb_follows(&sb, v); break;
    case V_GIFT: sb_gift(&sb, v); break;
    default: sb_report(&sb, v); break;
    }
    char *html = sb_finish(&sb);
    if (S.scenario) {
        free(g_dump_views);
        g_dump_views = strdup(html);
        dump_state();
    }
    Keep k;
    static const char *const none[1] = {""};
    keep_save(&k, none, 0);
    v10_view_set(html);
    keep_restore(&k);
}

// ------------------------------------------------------------------ actions

static Feed *feed_with(const int64_t pid, cJSON **post) {
    for (int i = 0; i < FEED_TABS; i++) {
        cJSON *p;
        cJSON_ArrayForEach(p, S.feeds[i].posts) if ((int64_t) jnum(p, "pid") == pid) {
            *post = p;
            return &S.feeds[i];
        }
    }
    *post = NULL;
    return NULL;
}

static void set_num(cJSON *o, const char *k, const double v) {
    cJSON_DeleteItemFromObject(o, k);
    cJSON_AddNumberToObject(o, k, v);
}

static void set_str(cJSON *o, const char *k, const char *v) {
    cJSON_DeleteItemFromObject(o, k);
    if (v) cJSON_AddStringToObject(o, k, v);
}

// every copy of a post (each feed, the profile feed, the open detail)
static int post_copies(const int64_t pid, cJSON **out, const int max) {
    int n = 0;
    for (int i = 0; i < FEED_TABS; i++) {
        cJSON *p;
        cJSON_ArrayForEach(p, S.feeds[i].posts) if ((int64_t) jnum(p, "pid") == pid && n < max) out[n++] = p;
    }
    for (int i = 0; i < S.nviews; i++) {
        cJSON *p;
        cJSON_ArrayForEach(p, S.views[i].feed) if ((int64_t) jnum(p, "pid") == pid && n < max) out[n++] = p;
        cJSON *d = cJSON_GetObjectItem(S.views[i].data, "post");
        if (S.views[i].type == V_POST && d && (int64_t) jnum(d, "pid") == pid && n < max) out[n++] = d;
    }
    return n;
}

// A follow moved `name`'s follower count on the server: every copy here moves with it, the posts
// by them and their profile views, so a post and the profile agree (follower_counts.h).
static void followers_moved(const char *name, const int delta) {
    if (!name || !name[0]) return;
    for (int i = 0; i < FEED_TABS; i++) follower_count_bump_posts(S.feeds[i].posts, name, delta);
    for (int i = 0; i < S.nviews; i++) {
        follower_count_bump_posts(S.views[i].feed, name, delta);
        follower_count_bump_profile(S.views[i].data, name, delta);
        if (S.views[i].type == V_POST) follower_count_bump_post(cJSON_GetObjectItem(S.views[i].data, "post"), name, delta);
    }
}

static void rerender_all(void) {
    if (S.open) render();
    render_views();
}

static void vote_done(const int status, const cJSON *data, const cJSON *error, const intptr_t tag) {
    (void) data;
    const int64_t pid = (int64_t) (tag >> 1);
    const int value = (int) (tag & 1);
    cJSON *copies[16];
    const int n = post_copies(pid, copies, 16);
    for (int i = 0; i < n; i++) {
        cJSON_DeleteItemFromObject(copies[i], "_busy");
        if (!ok(status, error)) { // roll back
            set_num(copies[i], "vot", jnum(copies[i], "vot") - (value ? 1 : -1));
            set_num(copies[i], "dlk", value ? 0 : 1);
            set_str(copies[i], "_voteErr", v10_gapi_error_text(error, "Could not vote"));
        }
    }
    rerender_all();
}

// _35282: optimistic like / unlike
static void post_vote(const int64_t pid) {
    cJSON *copies[16];
    const int n = post_copies(pid, copies, 16);
    if (!n || !actor() || jtrue(copies[0], "_busy")) return;
    const int value = (int) jnum(copies[0], "dlk") == 1 ? 0 : 1;
    for (int i = 0; i < n; i++) {
        set_num(copies[i], "vot", jnum(copies[i], "vot") + (value ? 1 : -1));
        set_num(copies[i], "dlk", value);
        cJSON_AddTrueToObject(copies[i], "_busy");
        set_str(copies[i], "_voteErr", NULL);
    }
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "value", value);
    char path[64];
    snprintf(path, sizeof(path), "/social/post/%lld/vote", (long long) pid);
    if (!gapi("POST", path, body, vote_done, (intptr_t) (pid << 1 | value))) vote_done(0, NULL, NULL, (intptr_t) (pid << 1 | value));
    cJSON_Delete(body);
    rerender_all();
}

static void post_follow_done(const int status, const cJSON *data, const cJSON *error, const intptr_t tag) {
    (void) data;
    cJSON *copies[16];
    const int n = post_copies((int64_t) tag, copies, 16);
    for (int i = 0; i < n; i++) {
        cJSON_DeleteItemFromObject(copies[i], "_following");
        if (ok(status, error)) cJSON_AddTrueToObject(copies[i], "_followed");
    }
    if (n && ok(status, error)) followers_moved(jstr(copies[0], "pln"), 1); // the author gained a follower
    rerender_all();
}

// _35276: the giveaway's Follow button (local state only)
static void post_follow(const int64_t pid) {
    cJSON *copies[16];
    const int n = post_copies(pid, copies, 16);
    if (!n || !actor() || jtrue(copies[0], "_following") || jtrue(copies[0], "_followed")) return;
    for (int i = 0; i < n; i++) cJSON_AddTrueToObject(copies[i], "_following");
    cJSON *body = cJSON_CreateObject();
    cJSON_AddTrueToObject(body, "follow");
    char enc[96], path[160];
    backend_url_encode(enc, sizeof(enc), jstr(copies[0], "pln"));
    snprintf(path, sizeof(path), "/players/%s/follow", enc);
    if (!gapi("POST", path, body, post_follow_done, (intptr_t) pid)) post_follow_done(0, NULL, NULL, (intptr_t) pid);
    cJSON_Delete(body);
    rerender_all();
}

static void profile_follow_done(const int status, const cJSON *data, const cJSON *error, const intptr_t tag) {
    (void) data;
    View *v = view_of(tag);
    if (!v || !v->data) return;
    v->follow_busy = 0;
    if (!ok(status, error)) set_num(v->data, "follow_state", jnum(v->data, "follow_state") > 0 ? 0 : 1); // revert
    else followers_moved(jstr(v->data, "player_name"), jnum(v->data, "follow_state") > 0 ? 1 : -1); // their count, here too
    render_views();
}

static void profile_follow(View *v) {
    if (!v->data || v->follow_busy) return;
    const int now = jnum(v->data, "follow_state") > 0 ? 0 : 1;
    set_num(v->data, "follow_state", now);
    v->follow_busy = 1;
    cJSON *body = cJSON_CreateObject();
    cJSON_AddBoolToObject(body, "follow", now);
    char enc[96], path[160];
    backend_url_encode(enc, sizeof(enc), jstr(v->data, "player_name"));
    snprintf(path, sizeof(path), "/players/%s/follow", enc);
    if (!gapi("POST", path, body, profile_follow_done, view_tag(v))) profile_follow_done(0, NULL, NULL, view_tag(v));
    cJSON_Delete(body);
    render_views();
}

static void follow_row_done(const int status, const cJSON *data, const cJSON *error, const intptr_t tag) {
    (void) status;
    (void) data;
    (void) error;
    (void) tag;
    render_views();
}

static void follow_row(View *v, const char *name) {
    cJSON *list = v->follows_tab ? v->following : v->followers, *r;
    cJSON_ArrayForEach(r, list) {
        if (strcmp(jstr(r, "player_name"), name)) continue;
        const int now = jnum(r, "follow_state") > 0 ? 0 : 1;
        set_num(r, "follow_state", now);
        cJSON *body = cJSON_CreateObject();
        cJSON_AddBoolToObject(body, "follow", now);
        char enc[96], path[160];
        backend_url_encode(enc, sizeof(enc), name);
        snprintf(path, sizeof(path), "/players/%s/follow", enc);
        gapi("POST", path, body, follow_row_done, view_tag(v));
        cJSON_Delete(body);
        break;
    }
    render_views();
}

static void apply_done(const int status, const cJSON *data, const cJSON *error, const intptr_t tag) {
    (void) data;
    View *v = view_of(tag);
    if (!v) return;
    v->apply_busy = 0;
    if (ok(status, error)) v->applied = 1;
    else snprintf(v->apply_error, sizeof(v->apply_error), "%s", v10_gapi_error_text(error, "Failed to apply"));
    render_views();
}

static void comment_done(const int status, const cJSON *data, const cJSON *error, const intptr_t tag) {
    (void) data;
    View *v = view_of(tag);
    if (!v) return;
    if (!ok(status, error)) {
        v->sending = 0;
        snprintf(v->reply_error, sizeof(v->reply_error), "%s", v10_gapi_error_text(error, "Could not post comment"));
        render_views();
        return;
    }
    v->sending = 0;
    g_reply[0] = 0;
    post_reload(v); // the fresh post (counts) and comments
    render_views();
}

static void post_reply(View *v) {
    const char *t = g_reply;
    while (*t == ' ') t++;
    if (!*t || v->sending || !actor()) return;
    v->sending = 1;
    v->reply_error[0] = 0;
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "text", t);
    char path[64];
    snprintf(path, sizeof(path), "/social/post/%lld/comment", (long long) v->id);
    if (!gapi("POST", path, body, comment_done, view_tag(v))) comment_done(0, NULL, NULL, view_tag(v));
    cJSON_Delete(body);
    render_views();
}

static void create_done(const int status, const cJSON *data, const cJSON *error, const intptr_t tag) {
    View *v = view_of(tag);
    if (!v) return;
    if (!ok(status, error)) {
        v->submitting = 0;
        snprintf(v->compose_error, sizeof(v->compose_error), "%s", v10_gapi_error_text(error, "Could not create post"));
        render_views();
        return;
    }
    // pop with the locally built post: every feed pane prepends it
    const AccountInfo *a = active_account();
    const int giveaway = v->giveaway;
    cJSON *p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "pid", jnum(data, "post_id"));
    cJSON_AddNumberToObject(p, "pli", a ? a->id : 0);
    cJSON_AddStringToObject(p, "pln", a ? a->name : "");
    cJSON_AddNumberToObject(p, "prm", a ? a->premium : 0);
    cJSON_AddNumberToObject(p, "ftr", a ? a->featured : 0);
    cJSON_AddStringToObject(p, "cln", a ? a->clan : "");
    cJSON_AddNumberToObject(p, "clr", 0);
    cJSON_AddNumberToObject(p, "flw", a ? a->followers : 0);
    cJSON_AddNumberToObject(p, "tim", 0);
    cJSON_AddNumberToObject(p, "typ", giveaway ? 9 : 0);
    const char *t = g_compose_text;
    while (*t == ' ' || *t == '\n') t++;
    cJSON_AddStringToObject(p, "txt", t);
    cJSON_AddNumberToObject(p, "vot", 0);
    cJSON_AddNumberToObject(p, "dlk", 0);
    cJSON_AddNumberToObject(p, "cnt", 0);
    cJSON_AddNumberToObject(p, "exp", 0);
    cJSON_AddNumberToObject(p, "fds", giveaway ? atof(g_giveaway_amount) : 0);
    for (int i = 0; i < FEED_TABS; i++) {
        if (!S.feeds[i].posts) S.feeds[i].posts = cJSON_CreateArray();
        cJSON_InsertItemInArray(S.feeds[i].posts, 0, cJSON_Duplicate(p, 1));
    }
    cJSON_Delete(p);
    if (giveaway && cJSON_IsNumber(cJSON_GetObjectItem(data, "player_funds"))) account_set_funds(jnum(data, "player_funds"));
    g_compose_text[0] = 0;
    view_pop();
    if (S.open) render();
}

static void compose_submit(View *v) {
    const char *t = g_compose_text;
    while (*t == ' ' || *t == '\n') t++;
    const double amt = atof(g_giveaway_amount);
    if (v->submitting || !*t || (v->giveaway && amt < 10)) return;
    v->submitting = 1;
    v->compose_error[0] = 0;
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "type", v->giveaway ? 9 : 0);
    cJSON_AddStringToObject(body, "text", t);
    if (v->giveaway) cJSON_AddNumberToObject(body, "funds", amt);
    if (!gapi("POST", "/social/post", body, create_done, view_tag(v))) create_done(0, NULL, NULL, view_tag(v));
    cJSON_Delete(body);
    render_views();
}

static void social_frame_send(const char *label, cJSON *a1, cJSON *a2, cJSON *a3, cJSON *a4) {
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString(label));
    if (a1) cJSON_AddItemToArray(f, a1);
    if (a2) cJSON_AddItemToArray(f, a2);
    if (a3) cJSON_AddItemToArray(f, a3);
    if (a4) cJSON_AddItemToArray(f, a4);
    backend_social_send(f);
    cJSON_Delete(f);
}

static void gift_send(View *v) {
    const long long amt = atoll(g_gift_amount);
    v->error[0] = 0;
    if (amt < 10) {
        snprintf(v->error, sizeof(v->error), "Minimum gift is 10 KR");
        render_views();
        return;
    }
    v->loading = 1;
    // the economy's KR gift (services/brain/economy.c "kr"): v10 POST /players/<name>/gift
    social_frame_send("kr", cJSON_CreateString(v->name), cJSON_CreateNumber((double) amt), cJSON_CreateString(g_gift_msg), cJSON_CreateNull());
    render_views();
}

static void report_send(View *v) {
    const char *t = g_report_text;
    while (*t == ' ' || *t == '\n') t++;
    v->error[0] = 0;
    size_t len = strlen(t);
    while (len && (t[len - 1] == ' ' || t[len - 1] == '\n')) len--;
    if (len < 10) {
        snprintf(v->error, sizeof(v->error), "Please provide at least 10 characters of detail.");
        render_views();
        return;
    }
    v->loading = 1;
    social_frame_send("rpt", cJSON_CreateString(v->name), cJSON_CreateNumber(g_report_reason), cJSON_CreateString(t), NULL);
    render_views();
}

// replies of the hub socket frames the gift / report / sales / listings views use
static void on_view_frame(const char *label, const cJSON *frame) {
    View *v = top_view();
    if (!v) return;
    if (v->type == V_GIFT && v->loading && (!strcmp(label, "gir") || !strcmp(label, "po"))) {
        v->loading = 0;
        if (!strcmp(label, "gir")) {
            const cJSON *r = cJSON_GetArrayItem(frame, 1);
            v->done = 1;
            snprintf(v->message, sizeof(v->message), "Sent %.0f KR to %s!", jnum(r, "amount"), v->name);
            if (cJSON_IsNumber(cJSON_GetObjectItem(r, "funds"))) account_set_funds(jnum(r, "funds"));
        } else {
            const char *e = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 2));
            snprintf(v->error, sizeof(v->error), "%s", e && e[0] ? e : "Failed to send gift");
        }
        render_views();
    } else if (v->type == V_REPORT && v->loading && (!strcmp(label, "rptr") || !strcmp(label, "po"))) {
        v->loading = 0;
        if (!strcmp(label, "rptr") && cJSON_IsTrue(cJSON_GetArrayItem(frame, 1))) v->done = 1;
        else snprintf(v->error, sizeof(v->error), "%s", cJSON_GetStringValue(cJSON_GetArrayItem(frame, 2)) ? cJSON_GetStringValue(cJSON_GetArrayItem(frame, 2)) : "Failed to submit report");
        render_views();
    } else if (v->type == V_PROFILE && !strcmp(label, "psd") && v->sales_loading) {
        v->sales_loading = 0;
        cJSON_Delete(v->sales);
        v->sales = cJSON_Duplicate(cJSON_GetArrayItem(frame, 1), 1);
        if (!cJSON_IsArray(v->sales)) {
            cJSON_Delete(v->sales);
            v->sales = cJSON_CreateArray();
        }
        render_views();
    } else if (v->type == V_PROFILE && !strcmp(label, "ul") && v->listings_loading) {
        v->listings_loading = 0;
        cJSON_Delete(v->listings);
        v->listings = cJSON_Duplicate(cJSON_GetArrayItem(frame, 1), 1);
        if (!cJSON_IsArray(v->listings)) {
            cJSON_Delete(v->listings);
            v->listings = cJSON_CreateArray();
        }
        render_views();
    }
}

// krunker.io: v10's own calls (_37210 getSales(name, rangeIndex) -> [{d, f}]; getListings(name) ->
// [{marketid, skinindex, funds, m}], v10:109908 / 110044), the listings adapted to kn_brain's "ul"
// rows ({sk, kr}) the Sales tab renders
static void kio_sales_done(const int status, const cJSON *data, const cJSON *error, const intptr_t tag) {
    View *v = view_of(tag);
    if (!v) return;
    v->sales_loading = 0;
    cJSON_Delete(v->sales);
    v->sales = ok(status, error) && cJSON_IsArray(data) ? cJSON_Duplicate(data, 1) : cJSON_CreateArray();
    render_views();
}

static void kio_listings_done(const int status, const cJSON *data, const cJSON *error, const intptr_t tag) {
    View *v = view_of(tag);
    if (!v) return;
    v->listings_loading = 0;
    cJSON_Delete(v->listings);
    v->listings = cJSON_CreateArray();
    const cJSON *e;
    if (ok(status, error)) {
        cJSON_ArrayForEach(e, data) {
            cJSON *row = cJSON_CreateObject();
            cJSON_AddNumberToObject(row, "sk", jnum(e, "skinindex"));
            cJSON_AddNumberToObject(row, "kr", jnum(e, "funds"));
            cJSON_AddNumberToObject(row, "id", jnum(e, "marketid"));
            cJSON_AddItemToArray(v->listings, row);
        }
    }
    render_views();
}

static void profile_sales_load(View *v) {
    static const int days[7] = {7, 14, 21, 30, 60, 90, 120};
    if (!v->data) return;
    v->sales_loading = 1;
    v->listings_loading = 1;
    if (kio()) {
        char enc[160], path[220];
        backend_url_encode(enc, sizeof(enc), jstr(v->data, "player_name"));
        snprintf(path, sizeof(path), "/players/%s/sales?range=%d", enc, v->sales_range);
        if (!gapi("GET", path, NULL, kio_sales_done, view_tag(v))) kio_sales_done(0, NULL, NULL, view_tag(v));
        snprintf(path, sizeof(path), "/players/%s/listings", enc);
        if (!gapi("GET", path, NULL, kio_listings_done, view_tag(v))) kio_listings_done(0, NULL, NULL, view_tag(v));
        return;
    }
    social_frame_send("pst", cJSON_CreateString(jstr(v->data, "player_name")), cJSON_CreateNumber(days[v->sales_range]), NULL, NULL);
    social_frame_send("uml", cJSON_CreateNumber(jnum(v->data, "player_id")), NULL, NULL, NULL);
}

static char g_copied[96];
static void copied_clear(void *user) {
    (void) user;
    g_copied[0] = 0;
}

static void share(const char *kind, const char *value) {
    char origin[128], enc[160], url[400];
    html_page_origin(origin, sizeof(origin));
    backend_url_encode(enc, sizeof(enc), value);
    snprintf(url, sizeof(url), "%s/?%s=%s", origin, kind, enc);
    html_clipboard_write(url);
    snprintf(g_copied, sizeof(g_copied), "%s:%s", kind, value);
    html_timer_add(1500, copied_clear, NULL);
}

// ------------------------------------------------------------------ handlers

static void ignore_answer(void *user, const int status, const cJSON *data, const cJSON *error) {
    (void) user; (void) status; (void) data; (void) error;
}
#define NULL_GAPI ignore_answer

static void hub_closed(void) {
    S.open = 0;
    g_generation++;
    for (int i = 0; i < FEED_TABS; i++) S.feeds[i].loading = 0;
    S.news.loading = 0;
    S.search_loading = 0;
}

int v10_social_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    if (!name) return -1;
    if (!strcmp(name, "openSocial")) {
        if (!ui_page_is_v10()) return -1; // the v7 page's hub (social_ui.c) keeps the name
        // window.openSocial(tab): v10's default is 0 (the page's menu item calls it with no argument)
        const int want = argc > 0 && (args[0].kind == HTML_ARG_NUM || args[0].kind == HTML_ARG_STR) ? (int) arg_num(args, argc, 0, 0) : 0;
        open_hub(want);
        return 1;
    }
    if (!strcmp(name, "openPlayerProfile")) {
        open_profile(arg_str(args, argc, 0));
        return 1;
    }
    if (!strcmp(name, "openClanView")) {
        open_clan(arg_str(args, argc, 0), 0);
        return 1;
    }
    if (!strcmp(name, "deleteFollower")) { // window 5's Remove Friend: stop following them
        const char *who = arg_str(args, argc, 0);
        if (who[0]) {
            cJSON *body = cJSON_CreateObject();
            cJSON_AddFalseToObject(body, "follow");
            char enc[96], path[160];
            backend_url_encode(enc, sizeof(enc), who);
            snprintf(path, sizeof(path), "/players/%s/follow", enc);
            v10_gapi("POST", path, body, NULL_GAPI, NULL);
            cJSON_Delete(body);
        }
        return 1;
    }
    if (!strcmp(name, "followPlayer")) { // the Inbox's Followers entry: Follow Back (v10 followPlayer)
        const char *who = arg_str(args, argc, 0);
        if (who[0]) {
            cJSON *body = cJSON_CreateObject();
            cJSON_AddTrueToObject(body, "follow");
            char enc[96], path[160];
            backend_url_encode(enc, sizeof(enc), who);
            snprintf(path, sizeof(path), "/players/%s/follow", enc);
            v10_gapi("POST", path, body, NULL_GAPI, NULL);
            cJSON_Delete(body);
        }
        return 1;
    }
    if (!strcmp(name, "openPost")) {
        open_post((int64_t) arg_num(args, argc, 0, 0));
        return 1;
    }
    if (!strcmp(name, "v10sInput")) {
        const char *what = arg_str(args, argc, 0), *value = arg_str(args, argc, 1);
        if (!value[0] && ev && ev->value) value = ev->value;
        if (!strcmp(what, "socialq")) snprintf(S.social_query, sizeof(S.social_query), "%s", value);
        else if (!strcmp(what, "friendq")) {
            snprintf(S.friend_filter, sizeof(S.friend_filter), "%s", value);
            render();
        } else if (!strcmp(what, "msg")) {
            const int was = S.compose[0] != 0;
            snprintf(S.compose, sizeof(S.compose), "%s", value);
            // the counter / send button follow the text without re-rendering the textarea
            const size_t chars = utf16_len(S.compose);
            const HtmlElem send = html_dom_by_id("v10sSend");
            const char *t = S.compose;
            while (*t == ' ' || *t == '\n' || *t == '\t') t++;
            if (send) {
                if (*t && chars <= EB_TEXT_MAX && S.eb_status == EB_CONNECTED) html_dom_remove_attr(send, "disabled");
                else html_dom_set_attr(send, "disabled", "");
            }
            const HtmlElem count = html_dom_query(0, ".conversation-char-count");
            if (count) {
                char n[16];
                snprintf(n, sizeof(n), "%d", EB_TEXT_MAX - (int) chars);
                html_dom_set_text(count, n);
                html_dom_set_class(count, "conversation-char-count-low", EB_TEXT_MAX - (int) chars <= 32);
            }
            (void) was;
        } else if (!strcmp(what, "search")) {
            snprintf(S.search_q, sizeof(S.search_q), "%s", value);
            search_changed();
            render();
        } else if (!strcmp(what, "reply")) {
            snprintf(g_reply, sizeof(g_reply), "%s", value);
            const HtmlElem b = html_dom_by_id("v10sReplyBtn");
            const char *t = g_reply;
            while (*t == ' ') t++;
            if (b && top_view() && !top_view()->sending) {
                if (*t) html_dom_remove_attr(b, "disabled");
                else html_dom_set_attr(b, "disabled", "");
            }
        } else if (!strcmp(what, "compose")) {
            snprintf(g_compose_text, sizeof(g_compose_text), "%s", value);
            render_views();
        } else if (!strcmp(what, "amount")) {
            snprintf(g_giveaway_amount, sizeof(g_giveaway_amount), "%s", value);
            render_views();
        } else if (!strcmp(what, "giftamt")) {
            snprintf(g_gift_amount, sizeof(g_gift_amount), "%s", value);
            render_views();
        } else if (!strcmp(what, "giftmsg")) snprintf(g_gift_msg, sizeof(g_gift_msg), "%s", value);
        else if (!strcmp(what, "reporttext")) snprintf(g_report_text, sizeof(g_report_text), "%s", value);
        else if (!strcmp(what, "reason")) g_report_reason = atoi(value);
        return 1;
    }
    if (!strcmp(name, "v10sKey")) {
        const char *what = arg_str(args, argc, 0);
        const int enter = ev && html_key_is_enter(ev->key);
        if (!enter) return 1;
        if (!strcmp(what, "socialq")) return v10_social_handle("v10s", ev, (HtmlArg[]) {{HTML_ARG_STR, 0, "socialsearch"}}, 1), 0;
        if (!strcmp(what, "msg") && !(ev->mods & 1)) { // Enter sends, Shift+Enter is a new line
            conv_send();
            render();
            const HtmlElem in = html_dom_by_id("v10sMsgInput");
            if (in) html_dom_focus(in);
            return 0;
        }
        if (!strcmp(what, "reply") && top_view() && top_view()->type == V_POST) {
            post_reply(top_view());
            return 0;
        }
        return 1;
    }
    if (strcmp(name, "v10s") != 0) return -1;
    const char *op = arg_str(args, argc, 0);
    View *v = top_view();
    // krunker.io account: the hub's writes are not ported (follow, like, comment, post, gift,
    // report, trade, clan apply): their buttons are hidden as for a v10 guest, and a stale or
    // scripted click does nothing (the stop keeps a card click from opening its post)
    static const char *const kio_writes[] = {"compose", "submit", "giveaway", "vote", "pfollow", "reply", "follow", "frow",
                                             "trade", "gift", "giftsend", "report", "reportsend", "apply"};
    for (size_t i = 0; kio() && i < sizeof(kio_writes) / sizeof(kio_writes[0]); i++)
        if (!strcmp(op, kio_writes[i])) {
            html_ui_stop_propagation();
            return 1;
        }
    if (!strcmp(op, "stop")) {
        html_ui_stop_propagation();
    } else if (!strcmp(op, "noop")) {
    } else if (!strcmp(op, "tab")) {
        S.tab = (int) arg_num(args, argc, 1, 0);
        mount_tab(S.tab);
        if (S.tab == TAB_SOCIAL) feeds_load(1); // the panes mount again
        if (S.tab == TAB_SEARCH && S.search_q[0] && !S.searched) search_changed();
        render();
    } else if (!strcmp(op, "sub")) {
        S.sub = (int) arg_num(args, argc, 1, 0);
        if (S.sub < 0 || S.sub > 5) S.sub = 0;
        if (S.sub == 5) feeds_load(0);
        render();
    } else if (!strcmp(op, "socialsearch")) {
        const char *q = S.social_query;
        while (*q == ' ') q++;
        if (strlen(q) >= 1) { // onSearch(q): the Search tab with the query
            snprintf(S.search_q, sizeof(S.search_q), "%s", q);
            S.tab = TAB_SEARCH;
            search_changed();
            render();
        }
    } else if (!strcmp(op, "compose")) {
        if (actor()) {
            View *c = view_push(V_COMPOSE);
            (void) c;
            g_compose_text[0] = 0;
            g_giveaway_amount[0] = 0;
            render_views();
            const HtmlElem t = html_dom_by_id("v10sComposeText");
            if (t) html_dom_focus(t);
        }
    } else if (!strcmp(op, "giveaway") && v && v->type == V_COMPOSE) {
        v->giveaway = !v->giveaway;
        render_views();
    } else if (!strcmp(op, "submit") && v && v->type == V_COMPOSE) {
        compose_submit(v);
    } else if (!strcmp(op, "post")) {
        open_post((int64_t) arg_num(args, argc, 1, 0));
    } else if (!strcmp(op, "vote")) {
        html_ui_stop_propagation();
        post_vote((int64_t) arg_num(args, argc, 1, 0));
    } else if (!strcmp(op, "pfollow")) {
        html_ui_stop_propagation();
        post_follow((int64_t) arg_num(args, argc, 1, 0));
    } else if (!strcmp(op, "share")) {
        html_ui_stop_propagation();
        share(arg_str(args, argc, 1), arg_str(args, argc, 2));
    } else if (!strcmp(op, "reply") && v && v->type == V_POST) {
        post_reply(v);
    } else if (!strcmp(op, "friend")) {
        conv_open((int64_t) arg_num(args, argc, 1, 0));
        render();
        const HtmlElem in = html_dom_by_id("v10sMsgInput");
        if (in) html_dom_focus(in);
    } else if (!strcmp(op, "more")) {
        eb_load_more();
        render();
    } else if (!strcmp(op, "earlier")) {
        conv_load_earlier();
        render();
    } else if (!strcmp(op, "send")) {
        conv_send();
        render();
    } else if (!strcmp(op, "emoji")) {
        // toggle: insert ':' at the caret (no emoji skins are served, so no candidates panel opens)
        const HtmlElem in = html_dom_by_id("v10sMsgInput");
        if (in && strlen(S.compose) + 1 < sizeof(S.compose)) {
            strcat(S.compose, ":");
            html_dom_set_value(in, S.compose);
            html_dom_focus(in);
        }
    } else if (!strcmp(op, "join")) {
        const char *game = arg_str(args, argc, 1);
        if (game[0]) v10_party_join_game(game); // switchServer(friend.gameInstanceId) (v10_party.c)
    } else if (!strcmp(op, "back")) {
        view_pop();
    } else if (!strcmp(op, "backdrop")) {
        html_ui_stop_propagation();
        view_pop();
    } else if (!strcmp(op, "ptab") && v && v->type == V_PROFILE) {
        v->tab = (int) arg_num(args, argc, 1, 0);
        if (v->tab != 3) ui_tooltip_cancel(); // leaving Badges drops its tooltip (deob-10.0.3.js:126021-126025)
        if (v->tab == 2 && !v->sales) profile_sales_load(v);
        if (v->tab == 7 && !v->reports && !v->reports_loading && v->data) profile_reports_load(v);
        render_views();
    } else if (!strcmp(op, "ctab") && v && v->type == V_PROFILE) { // the Creations sub-tabs
        const int t = (int) arg_num(args, argc, 1, 0);
        v->ctab = t >= 0 && t < 4 ? t : 0;
        render_views();
    } else if (!strcmp(op, "pskin") && v && v->type == V_PROFILE) { // a created skin: its item page (svelte-dmu4q2, deob-10.0.3.js:125918-125922)
        v10_market_wallet_item((int) arg_num(args, argc, 1, -1));
    } else if (!strcmp(op, "btip") && v && v->type == V_PROFILE) { // a badge's tooltip: its name and description (deob-10.0.3.js:125949-125975)
        const BadgeDef *b = badge_get((int) arg_num(args, argc, 1, -1));
        if (b && ev && ev->target) {
            StrBuf tip = {0};
            sb_append(&tip, "<div class='tooltipSub'><span style='color:#FBC02D'>");
            sb_append_esc(&tip, b->name && b->name[0] ? b->name : "Unknown");
            sb_append(&tip, "</span></div><div class='tooltipSub' style='font-style:italic;font-size:8px;max-width:300px;'>");
            sb_append_esc(&tip, b->desc ? b->desc : "");
            sb_append(&tip, "</div>");
            char *html = sb_finish(&tip);
            ui_tooltip_show(ev->target, html);
            free(html);
        }
    } else if (!strcmp(op, "btipx")) {
        ui_tooltip_cancel();
    } else if (!strcmp(op, "twitch") && v && v->type == V_PROFILE && v->data) { // window.open("https://twitch.tv/" + name)
        char enc[160], url[200];
        backend_url_encode(enc, sizeof(enc), jstr(v->data, "player_twitchname"));
        snprintf(url, sizeof(url), "https://twitch.tv/%s", enc);
        if (enc[0]) ui_open_external(url);
    } else if (!strcmp(op, "range") && v && v->type == V_PROFILE) {
        v->sales_range = (int) arg_num(args, argc, 1, 3);
        cJSON_Delete(v->sales);
        v->sales = NULL;
        profile_sales_load(v);
        render_views();
    } else if (!strcmp(op, "follow") && v && v->type == V_PROFILE) {
        profile_follow(v);
    } else if (!strcmp(op, "follows") && v && v->type == V_PROFILE && v->data) {
        open_follows(jstr(v->data, "player_name"), (int) arg_num(args, argc, 1, 0), jnum(v->data, "player_followed"), jnum(v->data, "player_following"));
    } else if (!strcmp(op, "ftab") && v && v->type == V_FOLLOWS) {
        v->follows_tab = (int) arg_num(args, argc, 1, 0) ? 1 : 0;
        render_views();
    } else if (!strcmp(op, "frow") && v && v->type == V_FOLLOWS) {
        follow_row(v, arg_str(args, argc, 1));
    } else if (!strcmp(op, "trade") && v && v->type == V_PROFILE && v->data) {
        // v10 pushes its trade popup (_36706); KrunkNative's trade flow is the economy's (store_ui / hub)
        char name_arg[80];
        snprintf(name_arg, sizeof(name_arg), "%s", jstr(v->data, "player_name"));
        html_ui_dispatch("openTrade", ev, (HtmlArg[]) {{HTML_ARG_STR, 0, name_arg}}, 1);
    } else if (!strcmp(op, "gift") && v && v->type == V_PROFILE && v->data) {
        char who[64];
        snprintf(who, sizeof(who), "%s", jstr(v->data, "player_name"));
        View *g = view_push(V_GIFT);
        snprintf(g->name, sizeof(g->name), "%s", who);
        snprintf(g_gift_amount, sizeof(g_gift_amount), "100");
        g_gift_msg[0] = 0;
        render_views();
    } else if (!strcmp(op, "giftsend") && v && v->type == V_GIFT) {
        gift_send(v);
    } else if (!strcmp(op, "report") && v && v->type == V_PROFILE && v->data) {
        char who[64];
        snprintf(who, sizeof(who), "%s", jstr(v->data, "player_name"));
        View *r = view_push(V_REPORT);
        snprintf(r->name, sizeof(r->name), "%s", who);
        g_report_text[0] = 0;
        g_report_reason = 0;
        render_views();
    } else if (!strcmp(op, "reportsend") && v && v->type == V_REPORT) {
        report_send(v);
    } else if (!strcmp(op, "apply") && v && v->type == V_CLAN && v->data && !v->applied && !v->apply_busy) {
        v->apply_busy = 1;
        v->apply_error[0] = 0;
        char path[64];
        snprintf(path, sizeof(path), "/clans/%lld/apply", (long long) jnum(v->data, "clan_id"));
        if (!gapi("POST", path, NULL, apply_done, view_tag(v))) apply_done(0, NULL, NULL, view_tag(v));
        render_views();
    } else if (!strcmp(op, "discord")) {
        char url[160];
        snprintf(url, sizeof(url), "https://discord.gg/%s", arg_str(args, argc, 1));
        html_ui_dispatch("openURL", ev, (HtmlArg[]) {{HTML_ARG_STR, 0, url}}, 1);
    }
    return 1;
}

int v10_social_key(const int key) {
    const View *v = top_view();
    if (!html_key_is_escape(key) || !v || v->depth != v10_view_depth()) return 0;
    view_pop();
    return 1;
}

// ------------------------------------------------------------------ per frame

static void on_social_frames(void *user, const cJSON *frame) {
    on_social(user, frame);
    const char *label = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 0));
    if (label) on_view_frame(label, frame);
}

static void menu_badge(void) {
    const HtmlElem title = html_dom_by_id("menuBtnSocial");
    if (!title) return;
    const int n = v10_social_unread_total();
    if (n == S.unread_shown && S.inited > 1) return;
    S.unread_shown = n;
    S.inited = 2;
    HtmlElem badge = html_dom_query(title, ".counterBadge");
    if (!n) {
        if (badge) html_dom_remove(badge);
        return;
    }
    if (!badge) {
        badge = html_dom_create("span", NULL);
        if (!badge) return;
        html_dom_set_attr(badge, "class", "counterBadge svelte-fgmdj8");
        html_dom_append(title, badge);
    }
    char text[16], aria[32];
    snprintf(text, sizeof(text), n > 99 ? "99+" : "%d", n);
    snprintf(aria, sizeof(aria), "%d unread", n);
    html_dom_set_attr(badge, "aria-label", aria);
    html_dom_set_text(badge, text);
}

// v10.0.3 loads earlier messages when the conversation is scrolled to within 80 px of the top (its scroll handler,
// deob-10.0.3.js:157784-157790, bound at 158232). The native engine has no scroll event: a scroll is a change of
// #v10sMessages' scrollTop since the last tick, so a short list that never scrolls keeps its button only, as in 10.0.3.
static void conv_scroll_tick(void) {
    const Conv *c = S.open && S.tab == TAB_FRIENDS && S.selected ? conv_get(S.selected, 0) : NULL;
    const HtmlElem m = c ? html_dom_by_id("v10sMessages") : 0;
    HtmlScroll sc;
    if (!m || !html_dom_get_scroll(m, &sc)) {
        S.msgs_top = -1.0f;
        return;
    }
    const int scrolled = S.msgs_peer == S.selected && S.msgs_top >= 0.0f && sc.top != S.msgs_top;
    S.msgs_peer = S.selected;
    S.msgs_top = sc.top;
    if (scrolled && sc.top <= 80.0f && c->has_more && !c->loading) {
        conv_load_earlier();
        render();
    }
}

void v10_social_tick(void) {
    if (!ui_page_is_v10()) return;
    if (!S.inited) {
        S.inited = 1;
        backend_social_listen(on_social_frames, NULL);
    }
    if (S.scenario) { // --render-scenario: the stand-in state stays
        dump_state();
        return;
    }
    v10_social_account_changed();
    if (S.open && !hub_shown()) hub_closed(); // clearPops() / another popup replaced it
    krunker_eb_poll(kio_event, NULL); // krunker.io's event bus (nothing queued otherwise)
    if (kio()) {
        if (S.account_id && S.eb_status == EB_ERROR && S.kio_retry_at && now_ms() >= S.kio_retry_at) kio_connect();
    } else if (S.account_id && S.eb_status == EB_DISCONNECTED && backend_social_is_open() && !S.list_requested) {
        eb_connected();
    }
    if (S.eb_status == EB_CONNECTED && now_ms() - S.last_ping > EB_PING_MS) {
        S.last_ping = now_ms();
        eb_send_type("presence:ping", NULL, 0);
    }
    conv_scroll_tick();
    menu_badge();
}

// ------------------------------------------------------------------ --render-scenario: state.social

// {tab, sub, feed:[posts], news:[html], friends:[...], selected, messages:[{text, sent, ts}], status,
//  search:{q, results}, view:{type:"profile"|"clan"|"post"|"compose", data, tab, ctab (the Creations sub-tab)}}
void v10_social_scenario(const cJSON *st) {
    if (!st) return;
    S.scenario = 1;
    S.tab = (int) jnum(st, "tab");
    S.sub = (int) jnum(st, "sub");
    const cJSON *feed = cJSON_GetObjectItem(st, "feed");
    for (int i = 0; i < FEED_TABS; i++) {
        S.feeds[i].loaded = 1;
        S.feeds[i].posts = cJSON_Duplicate(feed && i == S.sub ? feed : cJSON_GetObjectItem(st, "empty"), 1);
        if (!S.feeds[i].posts) S.feeds[i].posts = cJSON_CreateArray();
    }
    S.news.loaded = 1;
    S.news.posts = cJSON_Duplicate(cJSON_GetObjectItem(st, "news"), 1);
    if (!S.news.posts) S.news.posts = cJSON_CreateArray();
    const cJSON *friends = cJSON_GetObjectItem(st, "friends");
    S.eb_status = jstr(st, "status")[0] && !strcmp(jstr(st, "status"), "error") ? EB_ERROR : EB_CONNECTED;
    if (friends) S.friends = cJSON_Duplicate(friends, 1);
    S.selected = (int64_t) jnum(st, "selected");
    if (S.selected) {
        Conv *c = conv_get(S.selected, 1);
        const cJSON *m;
        if (c) {
            c->initialised = 1;
            cJSON_ArrayForEach(m, cJSON_GetObjectItem(st, "messages")) {
                Msg *x = conv_insert(c, c->n);
                if (!x) break;
                x->text = strdup(jstr(m, "text"));
                x->sent = jtrue(m, "sent");
                x->ts = jnum(m, "ts");
                x->status = MSG_DELIVERED;
            }
        }
    }
    const cJSON *search = cJSON_GetObjectItem(st, "search");
    if (search) {
        snprintf(S.search_q, sizeof(S.search_q), "%s", jstr(search, "q"));
        S.results = cJSON_Duplicate(cJSON_GetObjectItem(search, "results"), 1);
        S.searched = S.results != NULL;
    }
    show_hub(S.tab);
    const cJSON *view = cJSON_GetObjectItem(st, "view");
    if (view) {
        const char *type = jstr(view, "type");
        View *v = view_push(!strcmp(type, "clan") ? V_CLAN : !strcmp(type, "post") ? V_POST : !strcmp(type, "compose") ? V_COMPOSE : V_PROFILE);
        v->data = cJSON_Duplicate(cJSON_GetObjectItem(view, "data"), 1);
        v->tab = (int) jnum(view, "tab");
        v->ctab = (int) jnum(view, "ctab"); // the Creations sub-tab (0 Skins)
        if (v->type == V_PROFILE) v->feed = cJSON_CreateArray();
        render_views();
    }
}
