// Krunker v10.0.2's Studios on the v10 page: the Studios window (window.openStudios ->
// _5235(_49033).push(), v10.js 215744; component _49033, svelte-kqehnu, 138200-138440), the studio
// view it opens (_48488, svelte-a9j4oe, 137011-137610) and its monetization dialog (svelte-1veg8cv),
// plus the confirm / transfer prompts (_34410 / _6696 io-popups). All of them are entries of the
// page's one view stack (v10_view_push, v10_competitive.c); this module keeps each entry's state
// and re-renders its markup (v10_view_set) while it is the top one.
//
// The markup is v10's templates with the same classes (ui/css/svelte.css styles them); the events
// Svelte binds with __click / __input are inline knStudios(op, ...) handlers here, and the elements
// carry knSt* ids (KrunkNative-only, for the client e2e). Text is v10's i18n (studios.*).
// Data: v10's _10449.studios.* calls (v10.js 27360-27520) over the gapi tunnel of the social socket
// (v10_gapi.c -> services/brain/studios.c); the studio view's "Your Games" list is
// _10449.players.getProfile(me).maps, as in v10. v10's _10449.skins.list() (the Skin Manager's
// custom skins) has no KrunkNative counterpart: the "Your Skins" select stays empty.
// The Games window's My Games tab shows the pending invite count (servers_ui.c, the functions at
// the end).
#include <v10_studios.h>
#include <v10_social.h>
#include <v10_ui.h>
#include <backend.h>
#include <backend_ui.h>
#include <html_ui.h>
#include <i18n.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MSG 320

enum { TAB_MEMBERS, TAB_MAPS, TAB_SKINS, TAB_CURRENCIES, TAB_ANALYTICS, TAB_COUNT };
// R_ACTION flags: what a successful call also does
enum { AF_MON = 1, AF_INVITE_NAME = 2, AF_MAP_SEL = 4, AF_CURRENCY = 8 };
enum { CONFIRM_DELETE = 1, CONFIRM_REMOVE, CONFIRM_LEAVE, CONFIRM_REMOVE_MAP, CONFIRM_CANCEL_MON, CONFIRM_TRANSFER };

static struct {
    // the Studios window (_49033)
    int list_depth;          // its view-stack depth, 0 = closed
    unsigned list_gen;
    cJSON *studios, *invites; // getMyStudios / getMyStudioInvites data
    int loading;
    char error[MSG], success[MSG];
    char create_name[64];
    int creating;
    char create_error[MSG];
    int64_t invite_busy[8];  // invites being accepted / declined
    // the studio view (_48488)
    int sd_depth;
    unsigned sd_gen;
    int64_t sid;
    cJSON *studio;
    int sd_loading;
    char sd_error[MSG], sd_success[MSG];
    int tab;
    char invite_name[64];
    int inviting;
    cJSON *my_maps;          // [{mid, mna}] of players.getProfile(me)
    char map_sel[24];
    int assigning;
    cJSON *skins;            // getStudioSkins
    int skin_busy[4];
    char cur_name[64], cur_symbol[16];
    int cur_decimals, cur_creating;
    cJSON *mon;              // getStudioMonetization
    char mon_error[MSG];
    int mon_cancelling;
    cJSON *analytics;
    int an_loading;
    char an_error[MSG];
    // the monetization dialog (svelte-1veg8cv, inside the studio view)
    int dialog;
    char desc[2048];
    int agree, submitting;
    int64_t sel_maps[5];
    int nsel;
    char dlg_error[MSG];
    // the confirm / transfer prompt
    int confirm_depth, confirm_kind;
    int64_t confirm_id;
    char confirm_name[64], transfer_value[64];
    // My Games: the invite count
    int invite_count;
    double invite_polled;
} S;

// ------------------------------------------------------------------ helpers

static double jnum(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItem(o, k);
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    if (cJSON_IsString(v) && v->valuestring) return atof(v->valuestring);
    return 0.0;
}

static const char *jstr(const cJSON *o, const char *k) {
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(o, k));
    return s ? s : "";
}

static void set_json(cJSON **slot, const cJSON *v) {
    cJSON_Delete(*slot);
    *slot = v ? cJSON_Duplicate(v, 1) : NULL;
}

static void set_msg(char *dst, const char *text) { snprintf(dst, MSG, "%s", text ? text : ""); }

// v10's error text: the answer's error.message, else the action's own failure text
static void set_err(char *dst, const cJSON *error, const char *fallback) { set_msg(dst, error ? v10_gapi_error_text(error, fallback) : fallback); }

static int ok(const int status, const cJSON *error) { return status >= 200 && status < 300 && !error; }

static const char *me(void) {
    const AccountInfo *a = active_account();
    return a ? a->name : "";
}

static int is_owner(void) { return S.studio && (int) jnum(S.studio, "my_role") == 1; }

static size_t utf8_len(const char *s) {
    size_t n = 0;
    for (; s && *s; s++) n += ((unsigned char) *s & 0xC0) != 0x80;
    return n;
}

// trim(): the text without leading / trailing whitespace, into out
static void trimmed(char *out, const size_t n, const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    size_t len = strlen(s);
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\n' || s[len - 1] == '\r')) len--;
    if (len >= n) len = n - 1;
    memcpy(out, s, len);
    out[len] = 0;
}

// _48304: an ISO time -> toLocaleDateString(undefined, {year: numeric, month: short, day: numeric})
static void fmt_date(char *out, const size_t n, const char *iso) {
    out[0] = 0;
    int y, mo, d, h = 0, mi = 0, s = 0;
    if (!iso || sscanf(iso, "%d-%d-%d%*c%d:%d:%d", &y, &mo, &d, &h, &mi, &s) < 3) return;
    struct tm t = {0};
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_sec = s;
#ifdef _WIN32
    const time_t at = _mkgmtime(&t);
    struct tm lt;
    localtime_s(&lt, &at);
#else
    const time_t at = timegm(&t);
    struct tm lt;
    localtime_r(&at, &lt);
#endif
    static const char *const k_mon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    snprintf(out, n, "%s %d, %d", k_mon[lt.tm_mon < 0 || lt.tm_mon > 11 ? 0 : lt.tm_mon], lt.tm_mday, lt.tm_year + 1900);
}

// /^[a-zA-Z0-9_]{3,30}$/ (the window) and the currency rules (the studio view)
static int studio_name_ok(const char *s) {
    const size_t n = strlen(s);
    if (n < 3 || n > 30) return 0;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_')) return 0;
    return 1;
}

static int currency_ok(void) {
    char name[64], sym[16];
    trimmed(name, sizeof(name), S.cur_name);
    trimmed(sym, sizeof(sym), S.cur_symbol);
    const size_t nl = strlen(name), sl = strlen(sym);
    if (nl < 3 || nl > 20 || sl < 2 || sl > 4) return 0;
    for (const char *p = name; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == ' ')) return 0;
    for (const char *p = sym; *p; p++)
        if (*p < 'A' || *p > 'Z') return 0;
    return strcmp(sym, "KR") && strcmp(sym, "JNK") && strcmp(sym, "RP");
}

// ------------------------------------------------------------------ requests

typedef enum {
    R_MINE, R_INVITES, R_INVITE_ANSWER, R_CREATE, R_STUDIO, R_MY_MAPS, R_SKINS, R_MON, R_ACTION, R_LEAVE, R_DELETE,
    R_SKIN_TOGGLE, R_ANALYTICS, R_APPLY, R_MON_CANCEL, R_POLL_INVITES,
} ReqKind;

typedef struct {
    ReqKind kind;
    unsigned gen;
    int64_t id;
    int flag;
    char ok_text[MSG], fail_text[MSG];
} Req;

static void on_answer(void *user, int status, const cJSON *data, const cJSON *error);

static int send_req(const char *method, const char *path, cJSON *body, const ReqKind kind, const unsigned gen, const int64_t id,
                    const int flag, const char *ok_text, const char *fail_text) {
    Req *r = calloc(1, sizeof(*r));
    if (!r) {
        cJSON_Delete(body);
        return 0;
    }
    r->kind = kind;
    r->gen = gen;
    r->id = id;
    r->flag = flag;
    if (ok_text) set_msg(r->ok_text, ok_text);
    if (fail_text) set_msg(r->fail_text, fail_text);
    const int sent = v10_gapi(method, path, body, on_answer, r);
    cJSON_Delete(body);
    if (!sent) {
        on_answer(r, 0, NULL, NULL); // no brain: the failure path (frees r)
        return 0;
    }
    return 1;
}

static void render(void);

static void load_mine(void) {
    S.loading = 1;
    S.error[0] = 0;
    send_req("GET", "/studios/mine", NULL, R_MINE, S.list_gen, 0, 0, NULL, I18N("studios.error.load", "Failed to load your studios."));
}

static void load_invites(const ReqKind kind) { send_req("GET", "/studios/invites", NULL, kind, S.list_gen, 0, 0, NULL, NULL); }

static void load_studio(void) {
    char path[64];
    snprintf(path, sizeof(path), "/studios/%lld", (long long) S.sid);
    S.sd_loading = 1;
    S.sd_error[0] = 0;
    send_req("GET", path, NULL, R_STUDIO, S.sd_gen, 0, 0, NULL, I18N("studios.error.load", "Failed to load your studios."));
}

static void load_mon(void) {
    char path[80];
    snprintf(path, sizeof(path), "/studios/%lld/monetization", (long long) S.sid);
    S.mon_error[0] = 0;
    send_req("GET", path, NULL, R_MON, S.sd_gen, 0, 0, NULL, I18N("studios.error.monetization", "Failed to load the monetization status."));
}

static void load_skins(void) {
    char path[64];
    snprintf(path, sizeof(path), "/studios/%lld/skins", (long long) S.sid);
    send_req("GET", path, NULL, R_SKINS, S.sd_gen, 0, 0, NULL, NULL);
}

// _48555: run a studio call, show its success text and reload the studio, or show the error
static void action(const char *method, const char *path, cJSON *body, const int flags, const char *ok_text, const char *fail_text) {
    S.sd_error[0] = S.sd_success[0] = 0;
    send_req(method, path, body, R_ACTION, S.sd_gen, 0, flags, ok_text, fail_text);
    render();
}

static int busy_has(const int64_t *set, const int n, const int64_t id) {
    for (int i = 0; i < n; i++)
        if (set[i] == id) return 1;
    return 0;
}

static void busy_set(int64_t *set, const int n, const int64_t id, const int on) {
    for (int i = 0; i < n; i++) {
        if (on && !set[i]) {
            set[i] = id;
            return;
        }
        if (!on && set[i] == id) set[i] = 0;
    }
}

static void set_invite_count(const int n) { // window.setStudioInviteCount
    S.invite_count = n;
    const HtmlElem e = html_dom_by_id("studioInviteCount");
    if (!e) return;
    char t[16];
    snprintf(t, sizeof(t), "%d", n);
    html_dom_set_html(e, t);
    html_dom_set_display(e, n ? "inline-block" : "none");
}

static void open_studio(int64_t id);

static void on_answer(void *user, const int status, const cJSON *data, const cJSON *error) {
    Req *r = user;
    const int good = ok(status, error);
    const int list_live = S.list_depth && r->gen == S.list_gen, sd_live = S.sd_depth && r->gen == S.sd_gen;
    switch (r->kind) {
    case R_POLL_INVITES:
        if (good) set_invite_count(cJSON_GetArraySize(data));
        break;
    case R_MINE:
        if (!list_live) break;
        S.loading = 0;
        if (good) set_json(&S.studios, cJSON_IsArray(data) ? data : NULL);
        else {
            set_json(&S.studios, NULL);
            set_err(S.error, error, r->fail_text);
        }
        break;
    case R_INVITES:
        if (!list_live) break;
        set_json(&S.invites, good && cJSON_IsArray(data) ? data : NULL);
        set_invite_count(cJSON_GetArraySize(S.invites));
        break;
    case R_INVITE_ANSWER:
        if (!list_live) break;
        busy_set(S.invite_busy, 8, r->id, 0);
        if (good) {
            set_msg(S.success, r->ok_text);
            if (r->flag) load_mine();
        } else {
            set_err(S.error, error, r->fail_text);
        }
        load_invites(R_INVITES);
        break;
    case R_CREATE:
        if (!list_live) break;
        S.creating = 0;
        if (good) {
            S.create_name[0] = 0;
            load_mine();
            const int64_t id = (int64_t) jnum(data, "id");
            if (id > 0) open_studio(id);
        } else {
            set_err(S.create_error, error, r->fail_text);
        }
        break;
    case R_STUDIO:
        if (!sd_live) break;
        S.sd_loading = 0;
        if (good && cJSON_IsObject(data)) {
            set_json(&S.studio, data);
            if (!S.mon) load_mon();
        } else {
            set_json(&S.studio, NULL);
            set_err(S.sd_error, error, r->fail_text);
        }
        break;
    case R_MY_MAPS:
        if (!sd_live) break;
        cJSON_Delete(S.my_maps);
        S.my_maps = cJSON_CreateArray();
        if (good) {
            const cJSON *m;
            cJSON_ArrayForEach(m, cJSON_GetObjectItem(data, "maps")) {
                cJSON *o = cJSON_CreateObject();
                cJSON_AddNumberToObject(o, "mid", jnum(m, "mid"));
                cJSON_AddStringToObject(o, "mna", jstr(m, "mna"));
                cJSON_AddItemToArray(S.my_maps, o);
            }
        }
        break;
    case R_SKINS:
        if (!sd_live) break;
        set_json(&S.skins, good && cJSON_IsArray(data) ? data : NULL);
        break;
    case R_MON:
        if (!sd_live) break;
        if (good && cJSON_IsObject(data)) set_json(&S.mon, data);
        else {
            set_json(&S.mon, NULL);
            set_err(S.mon_error, error, r->fail_text);
        }
        if (r->flag && S.mon) S.dialog = 1; // _48342: Apply opens the dialog once the status is in
        break;
    case R_ACTION:
        if (!sd_live) break;
        if (good) {
            set_msg(S.sd_success, r->ok_text);
            if (r->flag & AF_MON) load_mon(); // a transfer: the new owner's requirements
            if (r->flag & AF_INVITE_NAME) S.invite_name[0] = 0;
            if (r->flag & AF_MAP_SEL) S.map_sel[0] = 0;
            if (r->flag & AF_CURRENCY) {
                S.cur_name[0] = S.cur_symbol[0] = 0;
                S.cur_decimals = 0;
            }
            load_studio();
        } else {
            set_err(S.sd_error, error, r->fail_text);
        }
        S.inviting = S.assigning = S.cur_creating = 0;
        break;
    case R_LEAVE:
    case R_DELETE:
        if (!sd_live) break;
        if (good) {
            if (S.sd_depth == v10_view_depth()) v10_view_pop(0); // _5221(): back to the Studios window
        } else {
            set_err(S.sd_error, error, r->fail_text);
        }
        break;
    case R_SKIN_TOGGLE:
        if (!sd_live) break;
        for (int i = 0; i < 4; i++)
            if (S.skin_busy[i] == (int) r->id) S.skin_busy[i] = 0;
        if (!good) set_err(S.sd_error, error, r->fail_text);
        load_skins();
        break;
    case R_ANALYTICS:
        if (!sd_live) break;
        S.an_loading = 0;
        if (good && cJSON_IsObject(data)) set_json(&S.analytics, data);
        else {
            set_json(&S.analytics, NULL);
            set_err(S.an_error, error, r->fail_text);
        }
        break;
    case R_APPLY:
        if (!sd_live) break;
        S.submitting = 0;
        if (good && cJSON_IsObject(data)) { // _48539 onapplied
            S.dialog = 0;
            set_json(&S.mon, data);
            S.sd_error[0] = 0;
            set_msg(S.sd_success, r->ok_text);
        } else {
            set_err(S.dlg_error, error, r->fail_text);
        }
        break;
    case R_MON_CANCEL:
        if (!sd_live) break;
        S.mon_cancelling = 0;
        if (good && cJSON_IsObject(data)) {
            set_json(&S.mon, data);
            set_msg(S.sd_success, r->ok_text);
        } else {
            set_err(S.sd_error, error, r->fail_text);
        }
        break;
    }
    free(r);
    render();
}

// ------------------------------------------------------------------ markup: the Studios window

static void sb_text(StrBuf *sb, const char *text) { sb_append_esc(sb, text ? text : ""); }

static void sb_msg(StrBuf *sb, const char *cls, const char *text) {
    if (!text || !text[0]) return;
    sb_appendf(sb, "<div class=\"%s\">", cls);
    sb_text(sb, text);
    sb_append(sb, "</div>");
}

static void list_markup(StrBuf *sb) {
    char buf[MSG], date[48], t2[MSG];
    sb_append(sb, "<div class=\"st-root svelte-kqehnu\" id=\"knStRoot\"><div class=\"st-header svelte-kqehnu\"><button class=\"st-back svelte-kqehnu\" "
                  "type=\"button\" aria-label=\"Back\" id=\"knStBack\" onclick=\"knStudios('back')\"><i class=\"material-icons svelte-kqehnu\">arrow_back</i></button> "
                  "<i class=\"material-icons st-header-icon svelte-kqehnu\">groups</i> <span class=\"st-header-title svelte-kqehnu\">");
    sb_text(sb, I18N("studios.title", "Studios"));
    sb_append(sb, "</span></div> <div class=\"st-body svelte-kqehnu\"><div class=\"st-intro svelte-kqehnu\">");
    sb_text(sb, I18N("studios.intro", "A studio is a team that shares your games. Studio members can work on the games you assign, and a studio can mint its own in-game currencies."));
    sb_append(sb, "</div> ");
    sb_msg(sb, "st-error svelte-kqehnu", S.error);
    sb_append(sb, " ");
    sb_msg(sb, "st-success svelte-kqehnu", S.success);
    sb_append(sb, " ");
    const int ninv = cJSON_GetArraySize(S.invites);
    if (ninv > 0) {
        sb_append(sb, "<div class=\"st-section-title st-section-first svelte-kqehnu\">");
        sb_text(sb, I18N("studios.invites.title", "Invites"));
        sb_appendf(sb, " (%d)</div> <div class=\"st-list svelte-kqehnu\">", ninv);
        const cJSON *inv;
        cJSON_ArrayForEach(inv, S.invites) {
            const int64_t id = (int64_t) jnum(inv, "invite_id");
            const int busy = busy_has(S.invite_busy, 8, id);
            sb_append(sb, "<div class=\"st-row st-invite svelte-kqehnu\"><i class=\"material-icons st-row-icon svelte-kqehnu\">mail</i> "
                          "<div class=\"st-row-info svelte-kqehnu\"><span class=\"st-row-name svelte-kqehnu\">");
            sb_text(sb, jstr(inv, "studio_name"));
            sb_append(sb, "</span> <span class=\"st-row-meta svelte-kqehnu\">");
            fmt_date(date, sizeof(date), jstr(inv, "expires_at"));
            I18N_F(buf, "studios.invites.from", "Invited by {0}", jstr(inv, "invited_by"));
            I18N_F(t2, "studios.invites.expires", "Expires {0}", date);
            sb_text(sb, buf);
            sb_append(sb, " \xc2\xb7 ");
            sb_text(sb, t2);
            sb_appendf(sb, "</span></div> <button class=\"st-mini st-mini-ok svelte-kqehnu\" type=\"button\" id=\"knStAccept%lld\" onclick=\"knStudios('answer',%lld,1)\"%s>",
                       (long long) id, (long long) id, busy ? " disabled" : "");
            sb_text(sb, I18N("studios.invites.accept", "Accept"));
            sb_appendf(sb, "</button> <button class=\"st-mini st-mini-danger svelte-kqehnu\" type=\"button\" id=\"knStDecline%lld\" onclick=\"knStudios('answer',%lld,0)\"%s>",
                       (long long) id, (long long) id, busy ? " disabled" : "");
            sb_text(sb, I18N("studios.invites.decline", "Decline"));
            sb_append(sb, "</button></div>");
        }
        sb_append(sb, "</div>");
    }
    sb_append(sb, " ");
    if (S.loading) {
        sb_append(sb, "<div class=\"st-center svelte-kqehnu\"><i class=\"material-icons spin svelte-kqehnu\">hourglass_empty</i></div>");
    } else if (cJSON_GetArraySize(S.studios) == 0) {
        sb_append(sb, "<div class=\"st-empty svelte-kqehnu\">");
        sb_text(sb, I18N("studios.empty", "You aren't in any studio yet."));
        sb_append(sb, "</div>");
    } else {
        sb_append(sb, "<div class=\"st-list svelte-kqehnu\">");
        const cJSON *s;
        cJSON_ArrayForEach(s, S.studios) {
            const int64_t id = (int64_t) jnum(s, "id");
            sb_appendf(sb, "<button class=\"st-row svelte-kqehnu\" type=\"button\" id=\"knStRow%lld\" onclick=\"knStudios('open',%lld)\">"
                           "<i class=\"material-icons st-row-icon svelte-kqehnu\">apartment</i> <div class=\"st-row-info svelte-kqehnu\"><span class=\"st-row-name svelte-kqehnu\">",
                       (long long) id, (long long) id);
            sb_text(sb, jstr(s, "name"));
            sb_append(sb, "</span> <span class=\"st-row-meta svelte-kqehnu\">");
            char count[24];
            snprintf(count, sizeof(count), "%d", (int) jnum(s, "member_count"));
            I18N_F(buf, "studios.row.owner", "Owner: {0}", jstr(s, "owner"));
            I18N_F(t2, "studios.row.members", "{0} members", count);
            sb_text(sb, buf);
            sb_append(sb, " \xc2\xb7 ");
            sb_text(sb, t2);
            sb_append(sb, "</span></div> ");
            if ((int) jnum(s, "role") == 1) {
                sb_append(sb, "<span class=\"st-badge svelte-kqehnu\">");
                sb_text(sb, I18N("studios.role.owner", "Owner"));
            } else {
                sb_append(sb, "<span class=\"st-badge st-badge-member svelte-kqehnu\">");
                sb_text(sb, I18N("studios.role.member", "Member"));
            }
            sb_append(sb, "</span> <i class=\"material-icons st-row-chevron svelte-kqehnu\">chevron_right</i></button>");
        }
        sb_append(sb, "</div>");
    }
    sb_append(sb, " <div class=\"st-section-title svelte-kqehnu\">");
    sb_text(sb, I18N("studios.create.title", "Create a Studio"));
    sb_append(sb, "</div> <div class=\"st-form svelte-kqehnu\"><label class=\"st-field svelte-kqehnu\"><span class=\"st-label svelte-kqehnu\">");
    sb_text(sb, I18N("studios.create.name", "Studio Name"));
    sb_append(sb, "</span> <input class=\"st-input svelte-kqehnu\" type=\"text\" maxlength=\"30\" id=\"knStCreateName\" placeholder=\"");
    sb_text(sb, I18N("studios.create.placeholder", "3-30 letters, numbers or _"));
    sb_append(sb, "\" value=\"");
    sb_text(sb, S.create_name);
    char name[64];
    trimmed(name, sizeof(name), S.create_name);
    sb_appendf(sb, "\" oninput=\"knStudios('input','create')\" onkeydown=\"knStudios('key','create')\"></label> <button class=\"st-primary svelte-kqehnu\" "
                   "type=\"button\" id=\"knStCreate\" onclick=\"knStudios('create')\"%s><i class=\"material-icons svelte-kqehnu\">add</i> ",
               S.creating || !studio_name_ok(name) ? " disabled" : "");
    sb_text(sb, S.creating ? I18N("studios.create.creating", "Creating...") : I18N("studios.create.submit", "Create"));
    sb_append(sb, "</button></div> <div class=\"st-hint svelte-kqehnu\">");
    sb_text(sb, I18N("studios.create.hint", "You can own up to 3 studios."));
    sb_append(sb, "</div> ");
    sb_msg(sb, "st-error svelte-kqehnu", S.create_error);
    sb_append(sb, "</div></div>");
}

// ------------------------------------------------------------------ markup: the studio view

static const char *mon_state(void) { // _48534
    if (!S.mon) return S.studio && cJSON_IsTrue(cJSON_GetObjectItem(S.studio, "monetization")) ? "enabled" : "off";
    if (cJSON_IsTrue(cJSON_GetObjectItem(S.mon, "enabled"))) return "enabled";
    const char *st = jstr(cJSON_GetObjectItem(S.mon, "request"), "status");
    if (!strcmp(st, "pending")) return "pending";
    if (cJSON_IsTrue(cJSON_GetObjectItem(S.mon, "blocked"))) return "blocked";
    if (!strcmp(st, "rejected")) return "rejected";
    if (!strcmp(st, "revoked")) return "revoked";
    return "off";
}

static int next_apply(char *date, const size_t n) {
    const char *at = jstr(S.mon, "next_apply_at");
    if (!at[0]) return 0;
    fmt_date(date, n, at);
    return 1;
}

static void members_markup(StrBuf *sb) {
    char buf[MSG], date[48];
    const int owner = is_owner();
    sb_append(sb, "<div class=\"sd-list svelte-a9j4oe\">");
    const cJSON *m;
    int i = 0;
    cJSON_ArrayForEach(m, cJSON_GetObjectItem(S.studio, "members")) {
        const int role = (int) jnum(m, "role");
        sb_append(sb, "<div class=\"sd-row svelte-a9j4oe\"><i class=\"material-icons sd-row-icon svelte-a9j4oe\">person</i> <span class=\"sd-row-name svelte-a9j4oe\">");
        sb_text(sb, jstr(m, "name"));
        sb_append(sb, "</span> ");
        if (role == 1) {
            sb_append(sb, "<span class=\"sd-badge svelte-a9j4oe\">");
            sb_text(sb, I18N("studios.role.owner", "Owner"));
            sb_append(sb, "</span>");
        }
        sb_append(sb, " ");
        if (owner && role != 1) {
            sb_appendf(sb, "<button class=\"sd-mini svelte-a9j4oe\" type=\"button\" id=\"knStMakeOwner%d\" onclick=\"knStudios('transfer',%d)\">", i, i);
            sb_text(sb, I18N("studios.members.makeOwner", "Make Owner"));
            sb_appendf(sb, "</button> <button class=\"sd-mini sd-mini-danger svelte-a9j4oe\" type=\"button\" id=\"knStRemove%d\" onclick=\"knStudios('remove',%d)\">", i, i);
            sb_text(sb, I18N("studios.members.remove", "Remove"));
            sb_append(sb, "</button>");
        } else if (!owner && !strcmp(me(), jstr(m, "name"))) {
            sb_appendf(sb, "<button class=\"sd-mini sd-mini-danger svelte-a9j4oe\" type=\"button\" id=\"knStLeave\" onclick=\"knStudios('remove',%d)\">", i);
            sb_text(sb, I18N("studios.members.leave", "Leave"));
            sb_append(sb, "</button>");
        }
        sb_append(sb, "</div>");
        i++;
    }
    sb_append(sb, "</div> ");
    const cJSON *invites = cJSON_GetObjectItem(S.studio, "invites");
    if (cJSON_GetArraySize(invites) > 0) {
        sb_append(sb, "<div class=\"sd-label svelte-a9j4oe\">");
        sb_text(sb, I18N("studios.members.pending", "Pending Invites"));
        sb_append(sb, "</div> <div class=\"sd-list svelte-a9j4oe\">");
        const cJSON *inv;
        cJSON_ArrayForEach(inv, invites) {
            const int64_t id = (int64_t) jnum(inv, "invite_id");
            sb_append(sb, "<div class=\"sd-row sd-row-off svelte-a9j4oe\"><i class=\"material-icons sd-row-icon svelte-a9j4oe\">schedule_send</i> <span class=\"sd-row-name svelte-a9j4oe\">");
            sb_text(sb, jstr(inv, "name"));
            sb_append(sb, "</span> <span class=\"sd-row-meta svelte-a9j4oe\">");
            fmt_date(date, sizeof(date), jstr(inv, "expires_at"));
            I18N_F(buf, "studios.members.inviteExpires", "Expires {0}", date);
            sb_text(sb, buf);
            sb_append(sb, "</span> ");
            if (owner) {
                sb_appendf(sb, "<button class=\"sd-mini sd-mini-danger svelte-a9j4oe\" type=\"button\" id=\"knStCancelInvite%lld\" onclick=\"knStudios('uninvite',%lld)\">",
                           (long long) id, (long long) id);
                sb_text(sb, I18N("studios.members.cancelInvite", "Cancel"));
                sb_append(sb, "</button>");
            }
            sb_append(sb, "</div>");
        }
        sb_append(sb, "</div>");
    }
    sb_append(sb, " ");
    if (owner) {
        char name[64];
        trimmed(name, sizeof(name), S.invite_name);
        sb_append(sb, "<div class=\"sd-form svelte-a9j4oe\"><label class=\"sd-field svelte-a9j4oe\"><span class=\"sd-label svelte-a9j4oe\">");
        sb_text(sb, I18N("studios.members.addLabel", "Player Name"));
        sb_append(sb, "</span> <input class=\"sd-input svelte-a9j4oe\" type=\"text\" id=\"knStInviteName\" placeholder=\"");
        sb_text(sb, I18N("studios.members.addPlaceholder", "Exact player name"));
        sb_append(sb, "\" value=\"");
        sb_text(sb, S.invite_name);
        sb_appendf(sb, "\" oninput=\"knStudios('input','invite')\" onkeydown=\"knStudios('key','invite')\"></label> <button class=\"sd-primary svelte-a9j4oe\" "
                       "type=\"button\" id=\"knStInvite\" onclick=\"knStudios('invite')\"%s><i class=\"material-icons svelte-a9j4oe\">person_add</i> ",
                   S.inviting || !name[0] ? " disabled" : "");
        sb_text(sb, I18N("studios.members.add", "Invite"));
        sb_append(sb, "</button></div> <div class=\"sd-hint svelte-a9j4oe\">");
        sb_text(sb, I18N("studios.members.hint", "A studio can hold up to 20 members, counting pending invites. Invites expire after 14 days."));
        sb_append(sb, "</div>");
    }
}

static void maps_markup(StrBuf *sb) {
    char buf[MSG], date[48];
    const char *state = mon_state();
    const int owner = is_owner(), enabled = !strcmp(state, "enabled");
    sb_appendf(sb, "<div class=\"sd-row sd-monetization svelte-a9j4oe%s\"><i class=\"material-icons sd-row-icon svelte-a9j4oe\">%s</i> <span class=\"sd-row-name svelte-a9j4oe\">",
               enabled ? "" : " sd-row-off", enabled ? "paid" : "money_off");
    sb_text(sb, I18N("studios.monetization.title", "Monetization"));
    sb_append(sb, "</span> ");
    if (enabled) {
        sb_append(sb, "<span class=\"sd-badge sd-badge-live svelte-a9j4oe\">");
        sb_text(sb, I18N("studios.monetization.on", "Enabled"));
        sb_append(sb, "</span>");
    } else if (!strcmp(state, "pending")) {
        sb_append(sb, "<span class=\"sd-badge svelte-a9j4oe\">");
        sb_text(sb, I18N("studios.monetization.pending", "Under review"));
        sb_append(sb, "</span> ");
        if (owner) {
            sb_appendf(sb, "<button class=\"sd-mini sd-mini-danger svelte-a9j4oe\" type=\"button\" id=\"knStMonCancel\" onclick=\"knStudios('moncancel')\"%s>",
                       S.mon_cancelling ? " disabled" : "");
            sb_text(sb, I18N("studios.monetization.cancel", "Cancel"));
            sb_append(sb, "</button>");
        }
    } else {
        const char *cls = "sd-badge sd-badge-down", *text;
        if (!strcmp(state, "rejected")) text = I18N("studios.monetization.rejected", "Rejected");
        else if (!strcmp(state, "revoked")) text = I18N("studios.monetization.revoked", "Revoked");
        else if (!strcmp(state, "blocked")) text = I18N("studios.monetization.blocked", "Blocked");
        else cls = "sd-badge sd-badge-plain", text = I18N("studios.monetization.off", "Not enabled");
        sb_appendf(sb, "<span class=\"%s svelte-a9j4oe\">", cls);
        sb_text(sb, text);
        sb_append(sb, "</span>");
    }
    sb_append(sb, " ");
    if (owner && S.mon && !next_apply(date, sizeof(date)) && (!strcmp(state, "off") || !strcmp(state, "rejected") || !strcmp(state, "revoked"))) {
        sb_append(sb, "<button class=\"sd-mini sd-mini-ok svelte-a9j4oe\" type=\"button\" id=\"knStMonApply\" onclick=\"knStudios('monapply')\">");
        sb_text(sb, I18N("studios.monetization.apply", "Apply for monetization"));
        sb_append(sb, "</button>");
    }
    sb_append(sb, "</div> ");
    sb_msg(sb, "sd-error svelte-a9j4oe", S.mon_error);
    sb_append(sb, " ");
    const char *note = jstr(cJSON_GetObjectItem(S.mon, "request"), "review_note");
    if ((!strcmp(state, "rejected") || !strcmp(state, "revoked") || !strcmp(state, "blocked")) && note[0]) {
        sb_append(sb, "<div class=\"sd-note svelte-a9j4oe\"><span class=\"sd-label svelte-a9j4oe\">");
        sb_text(sb, I18N("studios.monetization.note", "Note from the Krunker team"));
        sb_append(sb, "</span> <span class=\"sd-note-text svelte-a9j4oe\">");
        sb_text(sb, note);
        sb_append(sb, "</span></div>");
    }
    sb_append(sb, " <div class=\"sd-hint svelte-a9j4oe\">");
    if (enabled) sb_text(sb, I18N("studios.monetization.hintOn", "Games in this studio can charge KR through KR triggers and PAYMENTS.charge."));
    else if (!strcmp(state, "pending"))
        sb_text(sb, I18N("studios.monetization.hintPending", "The Krunker team is reviewing your application. You will get the decision by Krunker Mail."));
    else if (!strcmp(state, "blocked")) sb_text(sb, I18N("studios.monetization.hintBlocked", "This studio can no longer apply for monetization."));
    else if (!strcmp(state, "revoked") || !strcmp(state, "rejected")) {
        sb_text(sb, !strcmp(state, "revoked") ? I18N("studios.monetization.hintRevoked", "Monetization was turned off for this studio.")
                                              : I18N("studios.monetization.hintRejected", "Your application was not approved."));
        sb_append(sb, " ");
        if (next_apply(date, sizeof(date))) {
            I18N_F(buf, "studios.monetization.nextApply", "You can apply again on {0}.", date);
            sb_text(sb, buf);
        }
    } else
        sb_text(sb, I18N("studios.monetization.hintOff", "KR triggers and PAYMENTS.charge are disabled in this studio's games until the studio is approved for monetization."));
    sb_append(sb, "</div> ");
    const cJSON *maps = cJSON_GetObjectItem(S.studio, "maps");
    if (cJSON_GetArraySize(maps) == 0) {
        sb_append(sb, "<div class=\"sd-empty svelte-a9j4oe\">");
        sb_text(sb, I18N("studios.maps.empty", "No games assigned to this studio yet."));
        sb_append(sb, "</div>");
    } else {
        sb_append(sb, "<div class=\"sd-list svelte-a9j4oe\">");
        const cJSON *m;
        cJSON_ArrayForEach(m, maps) {
            const int64_t mid = (int64_t) jnum(m, "map_id");
            sb_append(sb, "<div class=\"sd-row svelte-a9j4oe\"><i class=\"material-icons sd-row-icon svelte-a9j4oe\">map</i> <span class=\"sd-row-name svelte-a9j4oe\">");
            sb_text(sb, jstr(m, "map_name"));
            sb_appendf(sb, "</span> <button class=\"sd-mini sd-mini-danger svelte-a9j4oe\" type=\"button\" id=\"knStMapRemove%lld\" onclick=\"knStudios('unmap',%lld)\">",
                       (long long) mid, (long long) mid);
            sb_text(sb, I18N("studios.maps.remove", "Remove"));
            sb_append(sb, "</button></div>");
        }
        sb_append(sb, "</div>");
    }
    sb_append(sb, " <div class=\"sd-form svelte-a9j4oe\"><label class=\"sd-field svelte-a9j4oe\"><span class=\"sd-label svelte-a9j4oe\">");
    sb_text(sb, I18N("studios.maps.addLabel", "Your Games"));
    sb_append(sb, "</span> <select class=\"sd-input svelte-a9j4oe\" id=\"knStMapSel\" onchange=\"knStudios('input','map')\"><option value=\"\">");
    sb_text(sb, I18N("studios.maps.addPlaceholder", "Select a game..."));
    sb_append(sb, "</option>");
    const cJSON *g;
    cJSON_ArrayForEach(g, S.my_maps) { // _48517: the ones not in the studio yet
        const int64_t mid = (int64_t) jnum(g, "mid");
        int taken = 0;
        const cJSON *m;
        cJSON_ArrayForEach(m, maps) if ((int64_t) jnum(m, "map_id") == mid) taken = 1;
        if (taken) continue;
        char v[24];
        snprintf(v, sizeof(v), "%lld", (long long) mid);
        sb_appendf(sb, "<option value=\"%s\"%s>", v, !strcmp(v, S.map_sel) ? " selected" : "");
        sb_text(sb, jstr(g, "mna"));
        sb_append(sb, "</option>");
    }
    sb_appendf(sb, "</select></label> <button class=\"sd-primary svelte-a9j4oe\" type=\"button\" id=\"knStAssignMap\" onclick=\"knStudios('map')\"%s>"
                   "<i class=\"material-icons svelte-a9j4oe\">add</i> ",
               S.assigning || !S.map_sel[0] ? " disabled" : "");
    sb_text(sb, I18N("studios.maps.add", "Assign Game"));
    sb_append(sb, "</button></div> <div class=\"sd-hint svelte-a9j4oe\">");
    sb_text(sb, I18N("studios.maps.hint", "A game can only belong to one studio at a time."));
    sb_append(sb, "</div>");
}

static void skins_markup(StrBuf *sb) {
    if (cJSON_GetArraySize(S.skins) == 0) {
        sb_append(sb, "<div class=\"sd-empty svelte-a9j4oe\">");
        sb_text(sb, I18N("studios.skins.empty", "No skins assigned to this studio yet."));
        sb_append(sb, "</div>");
    } else {
        sb_append(sb, "<div class=\"sd-list svelte-a9j4oe\">");
        const cJSON *k;
        cJSON_ArrayForEach(k, S.skins) {
            const int id = (int) jnum(k, "skin_id"), active = cJSON_IsTrue(cJSON_GetObjectItem(k, "active"));
            sb_appendf(sb, "<div class=\"sd-row svelte-a9j4oe%s\">", active ? "" : " sd-row-off");
            if (cJSON_IsTrue(cJSON_GetObjectItem(k, "has_thumbnail")) && jstr(k, "csrc")[0]) {
                sb_append(sb, "<img class=\"sd-skin-thumb svelte-a9j4oe\" src=\"https://user-assets.krunker.io/skins/");
                sb_text(sb, jstr(k, "csrc"));
                sb_append(sb, "/thumbnail.png\" alt=\"");
                sb_text(sb, jstr(k, "name"));
                sb_append(sb, "\">");
            } else {
                sb_append(sb, "<i class=\"material-icons sd-row-icon svelte-a9j4oe\">checkroom</i>");
            }
            sb_append(sb, " <span class=\"sd-row-name svelte-a9j4oe\">");
            sb_text(sb, jstr(k, "name"));
            sb_append(sb, "</span> <button class=\"sd-badge sd-badge-plain sd-skin-id svelte-a9j4oe\" type=\"button\" title=\"");
            sb_text(sb, I18N("studios.skins.idTitle", "Skin ID \xe2\x80\x94 use it in your scripts (setSkin, PAYMENTS.giveSkin). Click to copy."));
            sb_appendf(sb, "\" onclick=\"knStudios('copyskin',%d)\">ID %d</button> ", id, id);
            if (active) {
                sb_append(sb, "<span class=\"sd-badge sd-badge-live svelte-a9j4oe\">");
                sb_text(sb, I18N("studios.skins.published", "Published"));
            } else {
                sb_append(sb, "<span class=\"sd-badge sd-badge-down svelte-a9j4oe\">");
                sb_text(sb, I18N("studios.skins.unpublished", "Unpublished"));
            }
            sb_append(sb, "</span> ");
            if (is_owner()) {
                int busy = 0;
                for (int i = 0; i < 4; i++) busy |= S.skin_busy[i] == id;
                sb_appendf(sb, "<button type=\"button\" class=\"sd-mini %s svelte-a9j4oe\" onclick=\"knStudios('skin',%d)\"%s>",
                           active ? "sd-mini-danger" : "sd-mini-ok", id, busy ? " disabled" : "");
                sb_text(sb, active ? I18N("studios.skins.unpublish", "Unpublish") : I18N("studios.skins.publish", "Publish"));
                sb_append(sb, "</button>");
            }
            sb_append(sb, "</div>");
        }
        sb_append(sb, "</div>");
    }
    sb_append(sb, " ");
    if (!is_owner()) return;
    // _48469: "Your Skins" lists the Skin Manager's custom skins (_10449.skins.list()), which
    // KrunkNative does not have: only the placeholder option
    sb_append(sb, "<div class=\"sd-form svelte-a9j4oe\"><label class=\"sd-field svelte-a9j4oe\"><span class=\"sd-label svelte-a9j4oe\">");
    sb_text(sb, I18N("studios.skins.addLabel", "Your Skins"));
    sb_append(sb, "</span> <select class=\"sd-input svelte-a9j4oe\"><option value=\"\">");
    sb_text(sb, I18N("studios.skins.addPlaceholder", "Select a skin..."));
    sb_append(sb, "</option></select></label> <button class=\"sd-primary svelte-a9j4oe\" type=\"button\" disabled><i class=\"material-icons svelte-a9j4oe\">add</i> ");
    sb_text(sb, I18N("studios.skins.add", "Assign Skin"));
    sb_append(sb, "</button></div> <div class=\"sd-hint svelte-a9j4oe\">");
    sb_text(sb, I18N("studios.skins.hint", "Create skins in the Skin Manager, then assign them here. Assigned skins only appear inside this studio\xe2\x80\x99s games. Up to 20 per studio."));
    sb_append(sb, "</div>");
}

static void currencies_markup(StrBuf *sb) {
    char buf[MSG];
    const cJSON *list = cJSON_GetObjectItem(S.studio, "currencies");
    if (cJSON_GetArraySize(list) == 0) {
        sb_append(sb, "<div class=\"sd-empty svelte-a9j4oe\">");
        sb_text(sb, I18N("studios.currencies.empty", "No currencies created yet."));
        sb_append(sb, "</div>");
    } else {
        sb_append(sb, "<div class=\"sd-list svelte-a9j4oe\">");
        const cJSON *c;
        cJSON_ArrayForEach(c, list) {
            sb_append(sb, "<div class=\"sd-row svelte-a9j4oe\"><i class=\"material-icons sd-row-icon svelte-a9j4oe\">paid</i> <span class=\"sd-row-name svelte-a9j4oe\">");
            sb_text(sb, jstr(c, "name"));
            sb_append(sb, "</span> <span class=\"sd-badge sd-badge-plain svelte-a9j4oe\">");
            sb_text(sb, jstr(c, "symbol"));
            sb_append(sb, "</span> <span class=\"sd-row-meta svelte-a9j4oe\">");
            char d[16];
            snprintf(d, sizeof(d), "%d", (int) jnum(c, "decimals"));
            I18N_F(buf, "studios.currencies.decimals", "{0} decimals", d);
            sb_text(sb, buf);
            sb_append(sb, "</span></div>");
        }
        sb_append(sb, "</div>");
    }
    sb_append(sb, " ");
    if (!is_owner()) return;
    sb_append(sb, "<div class=\"sd-form svelte-a9j4oe\"><label class=\"sd-field svelte-a9j4oe\"><span class=\"sd-label svelte-a9j4oe\">");
    sb_text(sb, I18N("studios.currencies.name", "Name"));
    sb_append(sb, "</span> <input class=\"sd-input svelte-a9j4oe\" type=\"text\" maxlength=\"20\" id=\"knStCurName\" placeholder=\"");
    sb_text(sb, I18N("studios.currencies.namePlaceholder", "e.g. Gold Coins"));
    sb_append(sb, "\" value=\"");
    sb_text(sb, S.cur_name);
    sb_append(sb, "\" oninput=\"knStudios('input','curname')\"></label> <label class=\"sd-field sd-field-sm svelte-a9j4oe\"><span class=\"sd-label svelte-a9j4oe\">");
    sb_text(sb, I18N("studios.currencies.symbol", "Symbol"));
    sb_append(sb, "</span> <input class=\"sd-input svelte-a9j4oe\" type=\"text\" maxlength=\"4\" placeholder=\"GLD\" id=\"knStCurSymbol\" value=\"");
    sb_text(sb, S.cur_symbol);
    sb_append(sb, "\" oninput=\"knStudios('input','cursym')\"></label> <label class=\"sd-field sd-field-sm svelte-a9j4oe\"><span class=\"sd-label svelte-a9j4oe\">");
    sb_text(sb, I18N("studios.currencies.decimalsLabel", "Decimals"));
    sb_append(sb, "</span> <select class=\"sd-input svelte-a9j4oe\" id=\"knStCurDecimals\" onchange=\"knStudios('input','curdec')\">");
    for (int d = 0; d <= 2; d++) sb_appendf(sb, "<option value=\"%d\"%s>%d</option>", d, S.cur_decimals == d ? " selected" : "", d);
    sb_appendf(sb, "</select></label> <button class=\"sd-primary svelte-a9j4oe\" type=\"button\" id=\"knStCurCreate\" onclick=\"knStudios('currency')\"%s>"
                   "<i class=\"material-icons svelte-a9j4oe\">add</i> ",
               S.cur_creating || !currency_ok() ? " disabled" : "");
    sb_text(sb, I18N("studios.currencies.create", "Create"));
    sb_append(sb, "</button></div> <div class=\"sd-hint svelte-a9j4oe\">");
    sb_text(sb, I18N("studios.currencies.hint", "Symbols are 2-4 uppercase letters. KR, JNK and RP are reserved. Up to 5 currencies per studio."));
    sb_append(sb, "</div>");
}

static void analytics_markup(StrBuf *sb) {
    char buf[MSG], n1[48], n2[48];
    if (S.an_loading) {
        sb_append(sb, "<div class=\"sd-center svelte-a9j4oe\"><i class=\"material-icons spin svelte-a9j4oe\">hourglass_empty</i></div>");
        return;
    }
    if (S.an_error[0]) {
        sb_msg(sb, "sd-error svelte-a9j4oe", S.an_error);
        return;
    }
    if (!S.analytics) return;
    v10_fmt_int(n1, sizeof(n1), jnum(S.analytics, "kr_earned"));
    sb_append(sb, "<div class=\"sd-kr svelte-a9j4oe\" id=\"knStAnalytics\"><i class=\"material-icons sd-kr-icon svelte-a9j4oe\">savings</i> <div class=\"sd-kr-main svelte-a9j4oe\">"
                  "<span class=\"sd-kr-label svelte-a9j4oe\">");
    sb_text(sb, I18N("studios.analytics.krEarned", "KR Earned"));
    sb_appendf(sb, "</span> <span class=\"sd-kr-value svelte-a9j4oe\">%s</span></div> <span class=\"sd-kr-gross svelte-a9j4oe\">", n1);
    v10_fmt_int(n2, sizeof(n2), jnum(S.analytics, "kr_charged"));
    I18N_F(buf, "studios.analytics.krCharged", "Gross spent: {0} KR", n2);
    sb_text(sb, buf);
    sb_append(sb, "</span></div> <div class=\"sd-hint svelte-a9j4oe\">");
    sb_text(sb, I18N("studios.analytics.krHint", "Studios keep 90% of the KR players spend in their games."));
    sb_append(sb, "</div> ");
    const cJSON *cur = cJSON_GetObjectItem(S.analytics, "currencies");
    if (cJSON_GetArraySize(cur) == 0) {
        sb_append(sb, "<div class=\"sd-empty svelte-a9j4oe\">");
        sb_text(sb, I18N("studios.analytics.empty", "No currency activity yet."));
        sb_append(sb, "</div>");
        return;
    }
    sb_append(sb, "<div class=\"sd-list svelte-a9j4oe\"><div class=\"sd-row sd-row-head svelte-a9j4oe\"><span class=\"sd-row-name svelte-a9j4oe\">");
    sb_text(sb, I18N("studios.analytics.currency", "Currency"));
    sb_append(sb, "</span> <span class=\"sd-stat svelte-a9j4oe\">");
    sb_text(sb, I18N("studios.analytics.circulating", "Circulating"));
    sb_append(sb, "</span> <span class=\"sd-stat svelte-a9j4oe\">");
    sb_text(sb, I18N("studios.analytics.holders", "Holders"));
    sb_append(sb, "</span></div> ");
    const cJSON *c;
    cJSON_ArrayForEach(c, cur) {
        sb_append(sb, "<div class=\"sd-row svelte-a9j4oe\"><i class=\"material-icons sd-row-icon svelte-a9j4oe\">paid</i> <span class=\"sd-row-name svelte-a9j4oe\">");
        sb_text(sb, jstr(c, "name"));
        sb_append(sb, "</span> <span class=\"sd-badge sd-badge-plain svelte-a9j4oe\">");
        sb_text(sb, jstr(c, "symbol"));
        v10_fmt_int(n1, sizeof(n1), jnum(c, "circulating"));
        v10_fmt_int(n2, sizeof(n2), jnum(c, "holders"));
        sb_appendf(sb, "</span> <span class=\"sd-stat svelte-a9j4oe\">%s</span> <span class=\"sd-stat svelte-a9j4oe\">%s</span></div>", n1, n2);
    }
    sb_append(sb, "</div>");
}

// the monetization rules (_48924) and their i18n keys
static const struct {
    const char *title_key, *title, *text_key, *text;
} k_rules[9] = {
    {"studios.monetization.rule1.title", "Original work only.", "studios.monetization.rule1.text",
     "Your maps must be your studio's own work. No reuploads, ripped maps or barely changed templates."},
    {"studios.monetization.rule2.title", "Free to finish.", "studios.monetization.rule2.text",
     "Every map must be fully playable and completable without paying. Skips aren't allowed on maps that pay out KR rewards."},
    {"studios.monetization.rule3.title", "Clear prices.", "studios.monetization.rule3.text",
     "Always show the price and exactly what the player gets before they pay."},
    {"studios.monetization.rule4.title", "No gambling.", "studios.monetization.rule4.text",
     "Players must never pay for a chance to win KR, tradable items or anything worth something outside your map, and players can't bet against each other. "
     "Prize wheels may only award items and currencies that stay in your map, and must show the cost and the odds of every prize."},
    {"studios.monetization.rule5.title", "No paying to beat other players.", "studios.monetization.rule5.text",
     "Nothing a player buys may change a round against other players: no nukes, no damaging, slowing or penalising others, no team-wide effects. "
     "Upgrades and boosts are only allowed where players fight the map, not each other."},
    {"studios.monetization.rule6.title", "No entry fees.", "studios.monetization.rule6.text", "Never charge players to join, play or stay in a map."},
    {"studios.monetization.rule7.title", "No spam or pressure.", "studios.monetization.rule7.text",
     "Keep purchases in a store or clearly marked spots. No flooding the map with paid triggers or prompts, no fake timers, no misleading prompts, "
     "and no promising rewards you don't deliver."},
    {"studios.monetization.rule8.title", "Krunker's rules apply.", "studios.monetization.rule8.text",
     "Maps, names, thumbnails and studio skins must follow the Krunker Terms. No NSFW, hate, impersonation or stolen assets."},
    {"studios.monetization.rule9.title", "Enforcement.", "studios.monetization.rule9.text",
     "We can review your maps at any time. Breaking these rules can get monetization revoked. Serious or repeated breaks stop the studio and its owner from applying again."},
};
// I18N("studios.monetization.rule1.title", "Original work only.") ... the table above is read through i18n_tr

static int dialog_can_submit(void) { // _48932
    char d[2048];
    trimmed(d, sizeof(d), S.desc);
    const size_t n = utf8_len(d);
    return cJSON_IsTrue(cJSON_GetObjectItem(S.mon, "can_apply")) && S.agree && n >= 10 && n <= 500 && !S.submitting;
}

static int sel_has(const int64_t mid) {
    for (int i = 0; i < S.nsel; i++)
        if (S.sel_maps[i] == mid) return 1;
    return 0;
}

static void dialog_count(StrBuf *sb) { // the counter's content: the minimum note, then " N/500"
    char d[2048], buf[MSG];
    trimmed(d, sizeof(d), S.desc);
    const size_t n = utf8_len(d);
    if (n > 0 && n < 10) {
        I18N_F(buf, "studios.monetization.descMin", "At least {0} characters.", "10");
        sb_text(sb, buf);
    }
    sb_appendf(sb, " %d/500", (int) n);
}

static void dialog_markup(StrBuf *sb) {
    char buf[MSG], date[48];
    sb_append(sb, "<div class=\"sma-overlay svelte-1veg8cv\"><div class=\"sma-dialog svelte-1veg8cv\" role=\"dialog\" aria-modal=\"true\" id=\"knStMonDialog\">"
                  "<div class=\"sma-header svelte-1veg8cv\"><i class=\"material-icons sma-header-icon svelte-1veg8cv\">paid</i> <span class=\"sma-header-title svelte-1veg8cv\">");
    sb_text(sb, I18N("studios.monetization.applyTitle", "Apply for Monetization"));
    sb_append(sb, "</span> <button class=\"sma-close svelte-1veg8cv\" type=\"button\" aria-label=\"Close\" onclick=\"knStudios('monclose')\"><i class=\"material-icons\">close</i></button></div> "
                  "<div class=\"sma-body svelte-1veg8cv\"><div class=\"sma-section svelte-1veg8cv\">");
    sb_text(sb, I18N("studios.monetization.rulesTitle", "Rules"));
    sb_append(sb, "</div> <ol class=\"sma-rules svelte-1veg8cv\">");
    for (int i = 0; i < 9; i++) {
        sb_append(sb, "<li class=\"svelte-1veg8cv\"><b class=\"svelte-1veg8cv\">");
        sb_text(sb, i18n_tr(k_rules[i].title_key, k_rules[i].title));
        sb_append(sb, "</b> ");
        sb_text(sb, i18n_tr(k_rules[i].text_key, k_rules[i].text));
        sb_append(sb, "</li>");
    }
    sb_append(sb, "</ol> <div class=\"sma-section svelte-1veg8cv\">");
    sb_text(sb, I18N("studios.monetization.reqTitle", "Requirements"));
    sb_append(sb, "</div> <div class=\"sma-reqs svelte-1veg8cv\">");
    const cJSON *r;
    cJSON_ArrayForEach(r, cJSON_GetObjectItem(S.mon, "requirements")) {
        const char *key = jstr(r, "key");
        const int good = cJSON_IsTrue(cJSON_GetObjectItem(r, "ok"));
        char req[16], val[16];
        const cJSON *rq = cJSON_GetObjectItem(r, "required"), *vv = cJSON_GetObjectItem(r, "value");
        snprintf(req, sizeof(req), "%d", cJSON_IsNumber(rq) ? rq->valueint : 20);
        snprintf(val, sizeof(val), "%d", cJSON_IsNumber(vv) ? vv->valueint : 0);
        const char *text = key;
        if (!strcmp(key, "account_age")) text = I18N("studios.monetization.req.accountAge", "Account at least 3 months old");
        else if (!strcmp(key, "level") && cJSON_IsNumber(vv)) text = I18N_F(buf, "studios.monetization.req.levelYou", "Level {0} or higher (you are level {1})", req, val);
        else if (!strcmp(key, "level")) text = I18N_F(buf, "studios.monetization.req.level", "Level {0} or higher", req);
        else if (!strcmp(key, "twofa")) text = I18N("studios.monetization.req.twofa", "2FA enabled");
        else if (!strcmp(key, "no_recent_ban")) text = I18N("studios.monetization.req.noRecentBan", "No account bans in the last 6 months");
        else if (!strcmp(key, "has_map")) text = I18N("studios.monetization.req.hasMap", "At least one published map in this studio");
        sb_appendf(sb, "<div class=\"sma-req svelte-1veg8cv%s\"><i class=\"material-icons svelte-1veg8cv\">%s</i> <span>", good ? " sma-req-ok" : "",
                   good ? "check_circle" : "cancel");
        sb_text(sb, text);
        sb_append(sb, "</span></div>");
    }
    sb_append(sb, "</div> <label class=\"sma-field svelte-1veg8cv\"><span class=\"sma-label svelte-1veg8cv\">");
    sb_text(sb, I18N("studios.monetization.descLabel", "Tell us what you want to monetize"));
    sb_append(sb, "</span> <textarea class=\"sma-input sma-textarea svelte-1veg8cv\" rows=\"4\" maxlength=\"500\" id=\"knStMonDesc\" placeholder=\"");
    sb_text(sb, I18N("studios.monetization.descPlaceholder", "e.g. A cosmetic shop and a VIP lounge in our parkour map."));
    sb_append(sb, "\" oninput=\"knStudios('input','desc')\">");
    sb_text(sb, S.desc);
    char d[2048];
    trimmed(d, sizeof(d), S.desc);
    const size_t n = utf8_len(d);
    sb_appendf(sb, "</textarea> <span class=\"sma-count svelte-1veg8cv%s\" id=\"knStMonCount\">", n > 0 && (n < 10 || n > 500) ? " sma-count-bad" : "");
    dialog_count(sb);
    sb_append(sb, "</span></label> ");
    const cJSON *maps = cJSON_GetObjectItem(S.studio, "maps");
    if (cJSON_GetArraySize(maps) > 0) {
        sb_append(sb, "<div class=\"sma-field svelte-1veg8cv\"><span class=\"sma-label svelte-1veg8cv\">");
        I18N_F(buf, "studios.monetization.mapsLabel", "Example games (optional, up to {0})", "5");
        sb_text(sb, buf);
        sb_append(sb, "</span> <div class=\"sma-maps svelte-1veg8cv\">");
        const cJSON *m;
        cJSON_ArrayForEach(m, maps) {
            const int64_t mid = (int64_t) jnum(m, "map_id");
            const int on = sel_has(mid);
            sb_appendf(sb, "<button type=\"button\" class=\"sma-map svelte-1veg8cv%s\" id=\"knStMonMap%lld\" onclick=\"knStudios('monmap',%lld)\"%s>"
                           "<i class=\"material-icons svelte-1veg8cv\">%s</i> ",
                       on ? " active" : "", (long long) mid, (long long) mid, !on && S.nsel >= 5 ? " disabled" : "", on ? "check_box" : "check_box_outline_blank");
            sb_text(sb, jstr(m, "map_name"));
            sb_append(sb, "</button>");
        }
        sb_append(sb, "</div></div>");
    }
    sb_appendf(sb, " <label class=\"sma-agree svelte-1veg8cv\"><input type=\"checkbox\" class=\"svelte-1veg8cv\" id=\"knStMonAgree\" onchange=\"knStudios('input','agree')\"%s> <span>",
               S.agree ? " checked" : "");
    sb_text(sb, I18N("studios.monetization.agree", "I have read and agree to the rules"));
    sb_append(sb, "</span></label> ");
    // _48933: why Submit is off
    const char *hint = NULL;
    if (!cJSON_IsTrue(cJSON_GetObjectItem(S.mon, "can_apply"))) {
        if (cJSON_IsTrue(cJSON_GetObjectItem(S.mon, "blocked"))) hint = I18N("studios.monetization.hintBlocked", "This studio can no longer apply for monetization.");
        else if (!strcmp(jstr(cJSON_GetObjectItem(S.mon, "request"), "status"), "pending"))
            hint = I18N("studios.monetization.hintPending", "The Krunker team is reviewing your application. You will get the decision by Krunker Mail.");
        else if (next_apply(date, sizeof(date))) hint = I18N_F(buf, "studios.monetization.nextApply", "You can apply again on {0}.", date);
        else if (!cJSON_IsTrue(cJSON_GetObjectItem(S.mon, "eligible"))) hint = I18N("studios.monetization.notEligible", "Meet every requirement above to apply.");
    }
    if (hint) sb_msg(sb, "sma-hint svelte-1veg8cv", hint);
    sb_append(sb, " ");
    sb_msg(sb, "sma-error svelte-1veg8cv", S.dlg_error);
    sb_append(sb, "</div> <div class=\"sma-footer svelte-1veg8cv\"><button class=\"sma-secondary svelte-1veg8cv\" type=\"button\" id=\"knStMonClose\" onclick=\"knStudios('monclose')\">");
    sb_text(sb, I18N("studios.monetization.close", "Cancel"));
    sb_appendf(sb, "</button> <button class=\"sma-primary svelte-1veg8cv\" type=\"button\" id=\"knStMonSubmit\" onclick=\"knStudios('monsubmit')\"%s>"
                   "<i class=\"material-icons svelte-1veg8cv\">send</i> ",
               dialog_can_submit() ? "" : " disabled");
    sb_text(sb, I18N("studios.monetization.submit", "Submit"));
    sb_append(sb, "</button></div></div></div>");
}

static void studio_markup(StrBuf *sb) {
    sb_append(sb, "<div class=\"sd-root svelte-a9j4oe\" id=\"knStStudio\"><div class=\"sd-header svelte-a9j4oe\"><button class=\"sd-back svelte-a9j4oe\" type=\"button\" "
                  "aria-label=\"Back\" id=\"knStStudioBack\" onclick=\"knStudios('back')\"><i class=\"material-icons svelte-a9j4oe\">arrow_back</i></button> "
                  "<i class=\"material-icons sd-header-icon svelte-a9j4oe\">apartment</i> <span class=\"sd-header-title svelte-a9j4oe\">");
    const char *name = jstr(S.studio, "name");
    sb_text(sb, name[0] ? name : I18N("studios.title", "Studios"));
    sb_append(sb, "</span> ");
    if (is_owner()) {
        sb_append(sb, "<button class=\"sd-del svelte-a9j4oe\" type=\"button\" id=\"knStDelete\" onclick=\"knStudios('delete')\" title=\"");
        sb_text(sb, I18N("studios.delete.title", "Delete Studio"));
        sb_append(sb, "\"><i class=\"material-icons svelte-a9j4oe\">delete_forever</i></button>");
    }
    sb_append(sb, "</div> <div class=\"sd-body svelte-a9j4oe\">");
    sb_msg(sb, "sd-error svelte-a9j4oe", S.sd_error);
    sb_append(sb, " ");
    sb_msg(sb, "sd-success svelte-a9j4oe", S.sd_success);
    sb_append(sb, " ");
    if (S.sd_loading) {
        sb_append(sb, "<div class=\"sd-center svelte-a9j4oe\"><i class=\"material-icons spin svelte-a9j4oe\">hourglass_empty</i></div>");
    } else if (S.studio) {
        static const struct {
            const char *key, *text;
        } k_tabs[TAB_COUNT] = {{"studios.members.title", "Members"}, {"studios.maps.title", "Games"}, {"studios.skins.title", "Skins"},
                               {"studios.currencies.title", "Currencies"}, {"studios.analytics.title", "Analytics"}};
        // I18N("studios.members.title", "Members") I18N("studios.maps.title", "Games") I18N("studios.skins.title", "Skins")
        // I18N("studios.currencies.title", "Currencies") I18N("studios.analytics.title", "Analytics")
        sb_append(sb, "<div class=\"sd-tabs svelte-a9j4oe\">");
        for (int i = 0; i < TAB_COUNT; i++) {
            sb_appendf(sb, "%s<button class=\"sd-tab svelte-a9j4oe%s\" type=\"button\" id=\"knStTab%d\" onclick=\"knStudios('tab',%d)\">", i ? " " : "",
                       S.tab == i ? " active" : "", i, i);
            sb_text(sb, i18n_tr(k_tabs[i].key, k_tabs[i].text));
            sb_append(sb, "</button>");
        }
        sb_append(sb, "</div> ");
        switch (S.tab) {
        case TAB_MEMBERS: members_markup(sb); break;
        case TAB_MAPS: maps_markup(sb); break;
        case TAB_SKINS: skins_markup(sb); break;
        case TAB_CURRENCIES: currencies_markup(sb); break;
        default: analytics_markup(sb); break;
        }
    }
    sb_append(sb, "</div> ");
    if (S.dialog && S.mon) dialog_markup(sb);
    sb_append(sb, "</div>");
}

// ------------------------------------------------------------------ the view stack

// re-rendering replaces the markup: keep the focused knSt* field and its caret
static void set_view(char *html) {
    char focus[48] = "";
    int c0 = 0, c1 = 0, caret = 0;
    const HtmlElem a = html_dom_active_element();
    if (a && html_dom_get_attr(a, "id", focus, sizeof(focus)) >= 0 && !strncmp(focus, "knSt", 4)) caret = html_dom_text_selection(a, &c0, &c1);
    else focus[0] = 0;
    v10_view_set(html);
    if (focus[0]) {
        const HtmlElem e = html_dom_by_id(focus);
        if (e) {
            html_dom_focus(e);
            if (caret) html_dom_text_set_selection(e, c0, c1);
        }
    }
}

static void render(void) {
    const int top = v10_view_depth();
    if (!top) return;
    StrBuf sb = {0};
    if (top == S.sd_depth) studio_markup(&sb);
    else if (top == S.list_depth) list_markup(&sb);
    else return; // a prompt (static) or another module's view
    set_view(sb_finish(&sb));
}

static void studio_reset(void) {
    cJSON_Delete(S.studio);
    cJSON_Delete(S.my_maps);
    cJSON_Delete(S.skins);
    cJSON_Delete(S.mon);
    cJSON_Delete(S.analytics);
    S.studio = S.my_maps = S.skins = S.mon = S.analytics = NULL;
    S.sd_depth = 0;
    S.sd_gen++;
    S.sid = 0;
    S.sd_loading = S.inviting = S.assigning = S.cur_creating = S.mon_cancelling = S.an_loading = 0;
    S.dialog = S.agree = S.submitting = S.nsel = 0;
    S.sd_error[0] = S.sd_success[0] = S.mon_error[0] = S.an_error[0] = S.dlg_error[0] = 0;
    S.invite_name[0] = S.map_sel[0] = S.cur_name[0] = S.cur_symbol[0] = S.desc[0] = 0;
    S.cur_decimals = 0;
    S.tab = TAB_MEMBERS;
    memset(S.skin_busy, 0, sizeof(S.skin_busy));
}

static void list_closed(const int accepted, void *user) {
    (void) accepted;
    (void) user;
    cJSON_Delete(S.studios);
    cJSON_Delete(S.invites);
    S.studios = S.invites = NULL;
    S.list_depth = 0;
    S.list_gen++;
    S.loading = S.creating = 0;
    S.error[0] = S.success[0] = S.create_error[0] = S.create_name[0] = 0;
    memset(S.invite_busy, 0, sizeof(S.invite_busy));
}

static void studio_closed(const int accepted, void *user) {
    (void) accepted;
    (void) user;
    studio_reset();
    if (S.list_depth) { // the window's onClose: reload the list
        load_mine();
        render();
    }
}

static void open_list(void) { // window.openStudios
    if (S.list_depth && S.list_depth <= v10_view_depth()) {
        while (v10_view_depth() > S.list_depth) v10_view_pop(0);
    } else {
        v10_view_push(strdup(""), render, list_closed, NULL);
        S.list_depth = v10_view_depth();
        S.list_gen++;
    }
    load_mine();
    load_invites(R_INVITES);
    render();
}

static void open_studio(const int64_t id) {
    if (S.sd_depth) return;
    studio_reset();
    v10_view_push(strdup(""), render, studio_closed, NULL);
    S.sd_depth = v10_view_depth();
    S.sid = id;
    load_studio();
    if (me()[0]) {
        char path[96], enc[64];
        backend_url_encode(enc, sizeof(enc), me());
        snprintf(path, sizeof(path), "/players/%s", enc);
        send_req("GET", path, NULL, R_MY_MAPS, S.sd_gen, 0, 0, NULL, NULL);
    }
    load_skins();
    render();
}

// _34410 / _6696: the confirm prompt (fields: none) and the transfer form (one text field)
static void confirm_closed(const int accepted, void *user);

static void push_prompt(const int kind, const char *title, const char *desc, const char *confirm, const char *field_label, const char *placeholder) {
    StrBuf sb = {0};
    sb_append(&sb, "<div class=\"popup-container svelte-1wz7ymp\"><div class=\"popup svelte-1wz7ymp\" style=\"width: 800px;\"><div class=\"io-popup-container\">"
                   "<div class=\"io-popup-header\">");
    sb_text(&sb, title);
    sb_append(&sb, "</div> <div class=\"io-popup-content\"><div class=\"io-card\"><p>");
    sb_text(&sb, desc);
    sb_append(&sb, "</p> <form><div class=\"io-input-container\">");
    if (field_label) {
        sb_append(&sb, "<div class=\"label-row\"><label class=\"input-label\" for=\"knStTransferName\">");
        sb_text(&sb, field_label);
        sb_append(&sb, "</label></div> <input class=\"io-input-field\" id=\"knStTransferName\" type=\"text\" placeholder=\"");
        sb_text(&sb, placeholder);
        sb_append(&sb, "\" oninput=\"knStudios('input','transfer')\" onkeydown=\"knStudios('key','transfer')\"> ");
    }
    sb_append(&sb, "<div class=\"io-button-group\"><button type=\"button\" class=\"io-button\" id=\"knStConfirmCancel\" onclick=\"knViewPop(0)\">");
    sb_text(&sb, kind == CONFIRM_TRANSFER ? I18N("studios.transfer.cancel", "Cancel") : "Cancel");
    sb_appendf(&sb, "</button> <button type=\"button\" class=\"io-button io-button--accept\" id=\"knStConfirm\" onclick=\"knViewPop(1)\"%s>", field_label ? " disabled" : "");
    sb_text(&sb, confirm);
    sb_append(&sb, "</button></div></div></form></div></div></div></div></div>");
    S.confirm_kind = kind;
    S.transfer_value[0] = 0;
    v10_view_push(sb_finish(&sb), NULL, confirm_closed, NULL);
    S.confirm_depth = v10_view_depth();
    if (field_label) {
        const HtmlElem e = html_dom_by_id("knStTransferName");
        if (e) html_dom_focus(e);
    }
}

static void confirm_closed(const int accepted, void *user) {
    (void) user;
    const int kind = S.confirm_kind;
    S.confirm_depth = S.confirm_kind = 0;
    char path[160], enc[96], buf[MSG], fail[MSG];
    if (accepted && S.sd_depth && S.studio) {
        switch (kind) {
        case CONFIRM_DELETE:
            snprintf(path, sizeof(path), "/studios/%lld", (long long) S.sid);
            send_req("DELETE", path, NULL, R_DELETE, S.sd_gen, 0, 0, NULL, I18N("studios.error.delete", "Failed to delete the studio."));
            break;
        case CONFIRM_LEAVE:
        case CONFIRM_REMOVE:
            backend_url_encode(enc, sizeof(enc), S.confirm_name);
            snprintf(path, sizeof(path), "/studios/%lld/members/%s", (long long) S.sid, enc);
            set_msg(fail, I18N("studios.error.removeMember", "Failed to remove that member."));
            if (kind == CONFIRM_LEAVE) {
                S.sd_error[0] = 0;
                send_req("DELETE", path, NULL, R_LEAVE, S.sd_gen, 0, 0, NULL, fail);
            } else {
                I18N_F(buf, "studios.members.removed", "{0} was removed.", S.confirm_name);
                action("DELETE", path, NULL, 0, buf, fail);
            }
            break;
        case CONFIRM_REMOVE_MAP:
            snprintf(path, sizeof(path), "/studios/%lld/maps/%lld", (long long) S.sid, (long long) S.confirm_id);
            action("DELETE", path, NULL, 0, I18N("studios.maps.removed", "Game removed from the studio."), I18N("studios.error.removeMap", "Failed to remove that game."));
            break;
        case CONFIRM_CANCEL_MON:
            if (S.mon_cancelling) break;
            S.mon_cancelling = 1;
            S.sd_error[0] = S.sd_success[0] = 0;
            snprintf(path, sizeof(path), "/studios/%lld/monetization", (long long) S.sid);
            send_req("DELETE", path, NULL, R_MON_CANCEL, S.sd_gen, 0, 0, I18N("studios.monetization.cancelled", "Application withdrawn."),
                     I18N("studios.error.cancelApply", "Failed to withdraw the application."));
            break;
        case CONFIRM_TRANSFER: {
            char typed[64];
            trimmed(typed, sizeof(typed), S.transfer_value);
            if (strcmp(typed, jstr(S.studio, "name"))) {
                S.sd_success[0] = 0;
                set_msg(S.sd_error, I18N("studios.transfer.mismatch", "The studio name didn't match. Ownership was not transferred."));
                break;
            }
            cJSON *body = cJSON_CreateObject();
            cJSON_AddStringToObject(body, "player_name", S.confirm_name);
            snprintf(path, sizeof(path), "/studios/%lld/transfer", (long long) S.sid);
            I18N_F(buf, "studios.transfer.done", "{0} is now the owner of the studio.", S.confirm_name);
            S.sd_error[0] = S.sd_success[0] = 0;
            send_req("POST", path, body, R_ACTION, S.sd_gen, 0, AF_MON, buf, I18N("studios.error.transfer", "Failed to transfer the studio."));
            break;
        }
        default: break;
        }
    }
    render();
}

static const cJSON *member_at(const int i) { return cJSON_GetArrayItem(cJSON_GetObjectItem(S.studio, "members"), i); }

// ------------------------------------------------------------------ input (targeted updates, no re-render while typing)

static void set_disabled(const char *id, const int off) {
    const HtmlElem e = html_dom_by_id(id);
    if (!e) return;
    if (off) html_dom_set_attr(e, "disabled", "");
    else html_dom_remove_attr(e, "disabled");
}

// value: the inline handler's third argument when it has one (a scripted call), else this.value
static void on_input(const char *what, const HtmlEvent *ev, const char *value) {
    const char *v = value && value[0] ? value : ev && ev->value ? ev->value : "";
    char t[64];
    if (!strcmp(what, "create")) {
        snprintf(S.create_name, sizeof(S.create_name), "%s", v);
        trimmed(t, sizeof(t), v);
        set_disabled("knStCreate", S.creating || !studio_name_ok(t));
    } else if (!strcmp(what, "invite")) {
        snprintf(S.invite_name, sizeof(S.invite_name), "%s", v);
        trimmed(t, sizeof(t), v);
        set_disabled("knStInvite", S.inviting || !t[0]);
    } else if (!strcmp(what, "map")) {
        snprintf(S.map_sel, sizeof(S.map_sel), "%s", v);
        set_disabled("knStAssignMap", S.assigning || !S.map_sel[0]);
    } else if (!strcmp(what, "curname") || !strcmp(what, "cursym") || !strcmp(what, "curdec")) {
        if (!strcmp(what, "curname")) snprintf(S.cur_name, sizeof(S.cur_name), "%s", v);
        else if (!strcmp(what, "curdec")) S.cur_decimals = atoi(v) < 0 || atoi(v) > 2 ? 0 : atoi(v);
        else { // the symbol field upper-cases what is typed (_48474)
            snprintf(S.cur_symbol, sizeof(S.cur_symbol), "%s", v);
            int changed = 0;
            for (char *p = S.cur_symbol; *p; p++)
                if (*p >= 'a' && *p <= 'z') *p = (char) (*p - 32), changed = 1;
            if (changed && ev && ev->target) {
                int c0 = 0, c1 = 0;
                const int caret = html_dom_text_selection(ev->target, &c0, &c1);
                html_dom_set_value(ev->target, S.cur_symbol);
                if (caret) html_dom_text_set_selection(ev->target, c0, c1);
            }
        }
        set_disabled("knStCurCreate", S.cur_creating || !currency_ok());
    } else if (!strcmp(what, "desc")) {
        snprintf(S.desc, sizeof(S.desc), "%s", v);
        const HtmlElem count = html_dom_by_id("knStMonCount");
        if (count) {
            StrBuf sb = {0};
            dialog_count(&sb);
            char *text = sb_finish(&sb);
            html_dom_set_html(count, text);
            free(text);
            char d[2048];
            trimmed(d, sizeof(d), S.desc);
            const size_t n = utf8_len(d);
            html_dom_set_class(count, "sma-count-bad", n > 0 && (n < 10 || n > 500));
        }
        set_disabled("knStMonSubmit", !dialog_can_submit());
    } else if (!strcmp(what, "agree")) {
        S.agree = value && value[0] ? atoi(value) != 0 : ev ? ev->checked : !S.agree;
        set_disabled("knStMonSubmit", !dialog_can_submit());
    } else if (!strcmp(what, "transfer")) {
        snprintf(S.transfer_value, sizeof(S.transfer_value), "%s", v);
        trimmed(t, sizeof(t), v);
        set_disabled("knStConfirm", !t[0]);
    }
}

// ------------------------------------------------------------------ actions

static void do_create(void) { // _49065
    char name[64];
    trimmed(name, sizeof(name), S.create_name);
    if (S.creating) return;
    if (!studio_name_ok(name)) {
        set_msg(S.create_error, I18N("studios.error.name", "Use 3-30 letters, numbers or underscores."));
        render();
        return;
    }
    S.creating = 1;
    S.create_error[0] = 0;
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "name", name);
    send_req("POST", "/studios", body, R_CREATE, S.list_gen, 0, 0, NULL, I18N("studios.error.create", "Failed to create the studio."));
    render();
}

static void do_invite(void) { // _48561
    char name[64], path[64], buf[MSG];
    trimmed(name, sizeof(name), S.invite_name);
    if (!name[0] || S.inviting) return;
    S.inviting = 1;
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "player_name", name);
    snprintf(path, sizeof(path), "/studios/%lld/members", (long long) S.sid);
    I18N_F(buf, "studios.members.invited", "Invite sent to {0}.", name);
    action("POST", path, body, AF_INVITE_NAME, buf, I18N("studios.error.addMember", "Failed to invite that player."));
}

static void do_currency(void) { // _48364
    if (!currency_ok() || S.cur_creating) return;
    char name[64], sym[16], path[64];
    trimmed(name, sizeof(name), S.cur_name);
    trimmed(sym, sizeof(sym), S.cur_symbol);
    S.cur_creating = 1;
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "name", name);
    cJSON_AddStringToObject(body, "symbol", sym);
    cJSON_AddNumberToObject(body, "decimals", S.cur_decimals);
    snprintf(path, sizeof(path), "/studios/%lld/currencies", (long long) S.sid);
    action("POST", path, body, AF_CURRENCY, I18N("studios.currencies.created", "Currency created."), I18N("studios.error.createCurrency", "Failed to create that currency."));
}

static void do_assign_map(void) { // _48356
    if (!S.map_sel[0] || S.assigning) return;
    char path[64];
    S.assigning = 1;
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "map_id", atof(S.map_sel));
    snprintf(path, sizeof(path), "/studios/%lld/maps", (long long) S.sid);
    action("POST", path, body, AF_MAP_SEL, I18N("studios.maps.added", "Game assigned to the studio."), I18N("studios.error.addMap", "Failed to assign that game."));
}

static void do_submit(void) { // _48310
    if (!dialog_can_submit()) return;
    char d[2048], path[80];
    trimmed(d, sizeof(d), S.desc);
    S.submitting = 1;
    S.dlg_error[0] = 0;
    cJSON *body = cJSON_CreateObject(), *ids = cJSON_AddArrayToObject(body, "map_ids");
    cJSON_AddStringToObject(body, "description", d);
    for (int i = 0; i < S.nsel; i++) cJSON_AddItemToArray(ids, cJSON_CreateNumber((double) S.sel_maps[i]));
    snprintf(path, sizeof(path), "/studios/%lld/monetization", (long long) S.sid);
    send_req("POST", path, body, R_APPLY, S.sd_gen, 0, 0, I18N("studios.monetization.submitted", "Application sent. We'll let you know by Krunker Mail."),
             I18N("studios.error.apply", "Failed to send the application."));
    render();
}

static int64_t arg_i64(const HtmlArg *args, const int argc, const int i) {
    if (i >= argc) return 0;
    if (args[i].kind == HTML_ARG_NUM || args[i].kind == HTML_ARG_BOOL) return (int64_t) args[i].num;
    if (args[i].kind == HTML_ARG_STR && args[i].str) return strtoll(args[i].str, NULL, 10);
    return 0;
}

static const char *arg_s(const HtmlArg *args, const int argc, const int i) {
    return i < argc && args[i].kind == HTML_ARG_STR && args[i].str ? args[i].str : "";
}

int v10_studios_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    if (!name) return -1;
    if (!strcmp(name, "openStudios")) {
        open_list();
        return 1;
    }
    if (strcmp(name, "knStudios")) return -1;
    const char *op = arg_s(args, argc, 0);
    const int64_t a1 = arg_i64(args, argc, 1);
    char path[160], buf[MSG];
    if (!strcmp(op, "input")) {
        on_input(arg_s(args, argc, 1), ev, arg_s(args, argc, 2));
        return 1;
    }
    if (!strcmp(op, "key")) {
        if (!ev || !html_key_is_enter(ev->key)) return 1;
        const char *what = arg_s(args, argc, 1);
        if (!strcmp(what, "create")) do_create();
        else if (!strcmp(what, "invite")) do_invite();
        else if (!strcmp(what, "transfer") && S.confirm_depth == v10_view_depth()) {
            char t[64];
            trimmed(t, sizeof(t), S.transfer_value);
            if (t[0]) v10_view_pop(1); // the form submits on Enter
        }
        return 0;
    }
    if (!strcmp(op, "back")) { // _5221()
        const int top = v10_view_depth();
        if (top && (top == S.list_depth || top == S.sd_depth)) v10_view_pop(0);
        return 1;
    }
    if (!strcmp(op, "answer")) { // _49060
        if (!S.list_depth || busy_has(S.invite_busy, 8, a1)) return 1;
        const int accept = arg_i64(args, argc, 2) != 0;
        busy_set(S.invite_busy, 8, a1, 1);
        S.error[0] = S.success[0] = 0;
        const char *studio = "";
        const cJSON *inv;
        cJSON_ArrayForEach(inv, S.invites) if ((int64_t) jnum(inv, "invite_id") == a1) studio = jstr(inv, "studio_name");
        if (accept) I18N_F(buf, "studios.invites.accepted", "You joined {0}.", studio);
        else set_msg(buf, I18N("studios.invites.declined", "Invite declined."));
        snprintf(path, sizeof(path), "/studios/invites/%lld/%s", (long long) a1, accept ? "accept" : "decline");
        send_req("POST", path, NULL, R_INVITE_ANSWER, S.list_gen, a1, accept, buf,
                 accept ? I18N("studios.error.acceptInvite", "Failed to accept that invite.") : I18N("studios.error.declineInvite", "Failed to decline that invite."));
    } else if (!strcmp(op, "create")) {
        do_create();
        return 1;
    } else if (!strcmp(op, "open")) {
        open_studio(a1);
        return 1;
    }
    if (!S.sd_depth || !S.studio) {
        render();
        return 1;
    }
    // the studio view
    if (!strcmp(op, "tab")) {
        if (a1 < 0 || a1 >= TAB_COUNT) return 1;
        S.tab = (int) a1;
        if (S.tab == TAB_ANALYTICS && !S.analytics && !S.an_loading) { // _48577: loaded once
            S.an_loading = 1;
            S.an_error[0] = 0;
            snprintf(path, sizeof(path), "/studios/%lld/analytics", (long long) S.sid);
            send_req("GET", path, NULL, R_ANALYTICS, S.sd_gen, 0, 0, NULL, I18N("studios.analytics.error", "Failed to load analytics."));
        }
    } else if (!strcmp(op, "delete")) { // _48374
        if (!is_owner()) return 1;
        I18N_F(buf, "studios.delete.desc", "Delete \"{0}\"? Members lose access and its games are unassigned. This cannot be undone.", jstr(S.studio, "name"));
        push_prompt(CONFIRM_DELETE, I18N("studios.delete.title", "Delete Studio"), buf, I18N("studios.delete.confirm", "Delete"), NULL, NULL);
        return 1;
    } else if (!strcmp(op, "remove")) { // _48563
        const cJSON *m = member_at((int) a1);
        if (!m) return 1;
        snprintf(S.confirm_name, sizeof(S.confirm_name), "%s", jstr(m, "name"));
        if (!strcmp(S.confirm_name, me())) {
            I18N_F(buf, "studios.members.leaveDesc", "Leave \"{0}\"? You will lose access to its games.", jstr(S.studio, "name"));
            push_prompt(CONFIRM_LEAVE, I18N("studios.members.leaveTitle", "Leave Studio"), buf, I18N("studios.members.leave", "Leave"), NULL, NULL);
        } else {
            I18N_F(buf, "studios.members.removeDesc", "Remove {0} from this studio?", S.confirm_name);
            push_prompt(CONFIRM_REMOVE, I18N("studios.members.removeTitle", "Remove Member"), buf, I18N("studios.members.remove", "Remove"), NULL, NULL);
        }
        return 1;
    } else if (!strcmp(op, "transfer")) { // _48570
        const cJSON *m = member_at((int) a1);
        if (!m) return 1;
        snprintf(S.confirm_name, sizeof(S.confirm_name), "%s", jstr(m, "name"));
        I18N_F(buf, "studios.transfer.desc",
               "Make {0} the owner of \"{1}\"? You stay on as a member and only the new owner can transfer it back. Type the studio name to confirm.",
               S.confirm_name, jstr(S.studio, "name"));
        push_prompt(CONFIRM_TRANSFER, I18N("studios.transfer.title", "Transfer Ownership"), buf, I18N("studios.transfer.confirm", "Make Owner"),
                    I18N("studios.transfer.label", "Studio Name"), jstr(S.studio, "name"));
        return 1;
    } else if (!strcmp(op, "uninvite")) { // _48568
        const char *who = "";
        const cJSON *inv;
        cJSON_ArrayForEach(inv, cJSON_GetObjectItem(S.studio, "invites")) if ((int64_t) jnum(inv, "invite_id") == a1) who = jstr(inv, "name");
        I18N_F(buf, "studios.members.inviteCancelled", "Invite to {0} cancelled.", who);
        snprintf(path, sizeof(path), "/studios/%lld/invites/%lld", (long long) S.sid, (long long) a1);
        action("DELETE", path, NULL, 0, buf, I18N("studios.error.cancelInvite", "Failed to cancel that invite."));
        return 1;
    } else if (!strcmp(op, "invite")) {
        do_invite();
        return 1;
    } else if (!strcmp(op, "map")) {
        do_assign_map();
        return 1;
    } else if (!strcmp(op, "unmap")) { // _48574
        const char *map = "";
        const cJSON *m;
        cJSON_ArrayForEach(m, cJSON_GetObjectItem(S.studio, "maps")) if ((int64_t) jnum(m, "map_id") == a1) map = jstr(m, "map_name");
        S.confirm_id = a1;
        I18N_F(buf, "studios.maps.removeDesc", "Remove \"{0}\" from this studio? The game itself is not deleted.", map);
        push_prompt(CONFIRM_REMOVE_MAP, I18N("studios.maps.removeTitle", "Remove Game"), buf, I18N("studios.maps.remove", "Remove"), NULL, NULL);
        return 1;
    } else if (!strcmp(op, "currency")) {
        do_currency();
        return 1;
    } else if (!strcmp(op, "skin")) { // _48551
        const cJSON *k;
        int active = -1;
        cJSON_ArrayForEach(k, S.skins) if ((int64_t) jnum(k, "skin_id") == a1) active = cJSON_IsTrue(cJSON_GetObjectItem(k, "active"));
        if (active < 0) return 1;
        for (int i = 0; i < 4; i++) {
            if (S.skin_busy[i] == (int) a1) return 1;
        }
        for (int i = 0; i < 4; i++)
            if (!S.skin_busy[i]) {
                S.skin_busy[i] = (int) a1;
                break;
            }
        S.sd_error[0] = S.sd_success[0] = 0;
        snprintf(path, sizeof(path), "/studios/%lld/skins/%lld/%s", (long long) S.sid, (long long) a1, active ? "unpublish" : "publish");
        send_req("POST", path, NULL, R_SKIN_TOGGLE, S.sd_gen, a1, 0, NULL, I18N("studios.error.toggleSkin", "Failed to update that skin."));
    } else if (!strcmp(op, "copyskin")) { // _48549
        char id[24];
        snprintf(id, sizeof(id), "%lld", (long long) a1);
        html_clipboard_write(id);
        S.sd_error[0] = 0;
        I18N_F(S.sd_success, "studios.skins.idCopied", "Skin ID {0} copied.", id);
    } else if (!strcmp(op, "monapply")) { // _48342: the status first, then the dialog
        S.dlg_error[0] = 0;
        snprintf(path, sizeof(path), "/studios/%lld/monetization", (long long) S.sid);
        send_req("GET", path, NULL, R_MON, S.sd_gen, 0, 1, NULL, I18N("studios.error.monetization", "Failed to load the monetization status."));
        return 1;
    } else if (!strcmp(op, "monclose")) {
        S.dialog = 0;
    } else if (!strcmp(op, "monmap")) { // _48934
        int at = -1;
        for (int i = 0; i < S.nsel; i++)
            if (S.sel_maps[i] == a1) at = i;
        if (at >= 0) {
            memmove(&S.sel_maps[at], &S.sel_maps[at + 1], sizeof(S.sel_maps[0]) * (size_t) (S.nsel - at - 1));
            S.nsel--;
        } else if (S.nsel < 5) {
            S.sel_maps[S.nsel++] = a1;
        }
    } else if (!strcmp(op, "monsubmit")) {
        do_submit();
        return 1;
    } else if (!strcmp(op, "moncancel")) { // _48332
        if (S.mon_cancelling) return 1;
        push_prompt(CONFIRM_CANCEL_MON, I18N("studios.monetization.cancelTitle", "Cancel Application"),
                    I18N("studios.monetization.cancelDesc", "Withdraw your monetization application?"),
                    I18N("studios.monetization.cancelConfirm", "Withdraw"), NULL, NULL);
        return 1;
    }
    render();
    return 1;
}

int v10_studios_key(const int key) {
    if (!html_key_is_escape(key)) return 0;
    const int top = v10_view_depth();
    if (!top) return 0;
    if (top == S.confirm_depth) {
        v10_view_pop(0);
        return 1;
    }
    if (top == S.sd_depth && S.dialog) { // the open dialog closes first (_48488's keydown)
        S.dialog = 0;
        render();
        return 1;
    }
    if (top == S.sd_depth || top == S.list_depth) {
        v10_view_pop(0);
        return 1;
    }
    return 0;
}

// ------------------------------------------------------------------ My Games (servers_ui.c)

int v10_studios_invite_count(void) { return S.invite_count; }

// v10.js 213312: the My Games tab asks for the invites at most every 30 s, signed in only
void v10_studios_poll_invites(void) {
    if (!active_account() || backend_available() != 1) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const double now = (double) ts.tv_sec * 1000.0 + (double) ts.tv_nsec / 1e6;
    if (S.invite_polled > 0 && now - S.invite_polled < 30000) return;
    S.invite_polled = now;
    send_req("GET", "/studios/invites", NULL, R_POLL_INVITES, 0, 0, 0, NULL, NULL);
}
