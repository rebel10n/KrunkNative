// The K.P.D replay viewer (client/include/hud_replay.h, doc/v10/replays.md 2.5).
//
// Krunker v10.0.7 (deob-10.0.7.js):
//   watchDemo (:229694-229816)   download, load, close the socket, isReplay, start; onRoundStart closes the
//                                menu and follows the suspect (else spectMode(1)); the bar shows as flex
//   the bar (:229744-229806)     play / pause, the slider (its updates pause while held, the release seeks),
//                                "m:ss / m:ss", the speed button (0.5x 1x 2x 4x), the suspect button
//                                (follows it), the hitbox toggle, the resolve area, exit (a page reload)
//   the timeline (:229747-229763) the suspect segments painted yellow (#fbc02d) on the grey track
//   re-follow (:229810-229812)  every 1.5 s while playing, the camera goes back to the suspect
//   the resolve area (:229603-229660)  an open case: its counts and RESOLVE -> HACKER? YES / NO / cancel;
//                                kpd.vote(tok, ct, verdict) answers {yes, no, myv, banned, resolved}; a closed
//                                case: its status only; a failure: the error for 2.5 s; no call token: no vote
//   openDemoLink (:229818-229833) demoInfo(tok, ct) -> the case popup (map, mode, date, length, suspect,
//                                status, Watch, copy link); ?demo=<tok>&sus=<ct> opens it at load (:230167)
// Box Office: the brain is reached over HTTP with the account's session (?token=), the bar's text and CSS
// are our own; Exit Replay gives back the game the replay replaced instead of reloading the page.
#include <hud_replay.h>
#include <demo_play.h>
#include <ui_logic.h>
#include <html_dom.h>
#include <html_ui.h>
#include <backend.h>
#include <backend_ui.h>
#include <spectate.h>
#include <v10_ui.h> // v10_popup_show: the case popup (genericPopup)
#include <cJSON.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REPLAY_REFOLLOW_S 1.5   // :229812 (0x5dc ms)
#define REPLAY_VOTE_ERROR_MS 2500.0 // :229657 (0x9c4)
#define REPLAY_CASES 16

typedef struct {
    char key[100]; // tok|ct
    int known, opn, rsl, ban, yes, no, myv; // myv: -1 no vote
} ReplayCase;

static struct {
    Client *client;
    int bound;
    int shown;
    int dragging;        // the slider is held: its value is not overwritten (:229775)
    int hitboxes;
    int resolve_mode;    // 0 status, 1 the HACKER? question, 2 waiting for the vote, 3 an error shown
    int error_timer;
    double refollow_at;
    unsigned gen;        // a download / info answer for an older request is dropped
    char tok[48], ct[48];
    char last_time[32], last_speed[8], last_play[16], last_sus[48];
    uint32_t last_slider;
    ReplayCase cases[REPLAY_CASES];
    int case_next;
} g_rp;

int hud_replay_hitboxes(void) { return g_rp.hitboxes; }

// The bar's view: the replay player's, or a --render-scenario's fixed one (state.replayBar)
static struct {
    int on;
    uint32_t t, duration;
    float speed;
    int playing, suspect;
    DemoSegment segments[DEMO_SEGMENTS_MAX];
    size_t segment_count;
    char tok[48], ct[48];
} g_sc;

static int rv_active(void) { return g_sc.on || demo_play_active(); }
static uint32_t rv_playhead(void) { return g_sc.on ? g_sc.t : demo_play_playhead_ms(); }
static uint32_t rv_duration(void) { return g_sc.on ? g_sc.duration : demo_play_duration_ms(); }
static float rv_speed(void) { return g_sc.on ? g_sc.speed : demo_play_speed(); }
static int rv_playing(void) { return g_sc.on ? g_sc.playing : demo_play_playing(); }
static int rv_suspect(void) { return g_sc.on ? g_sc.suspect : demo_play_suspect_uid(); }
static const char *rv_token(void) { return g_sc.on ? g_sc.tok : demo_play_token(); }
static const char *rv_call_token(void) { return g_sc.on ? g_sc.ct : demo_play_call_token(); }
static const DemoSegment *rv_segments(size_t *count) {
    if (!g_sc.on) return demo_play_segments(count);
    *count = g_sc.segment_count;
    return g_sc.segments;
}

static void rp_set_text(const char *id, const char *text, char *last, const size_t last_size) {
    if (last && !strcmp(last, text)) return;
    const HtmlElem e = html_dom_by_id(id);
    if (e) html_dom_set_text(e, text);
    if (last) snprintf(last, last_size, "%s", text);
}

static void rp_time_text(char *out, const size_t size, const uint32_t ms) { // :229857-229860 m:ss
    const uint32_t s = ms / 1000u;
    snprintf(out, size, "%u:%02u", s / 60u, s % 60u);
}

static int rp_session_url(char *out, const size_t n, const char *path) {
    const char *t = account_session_token();
    if (!backend_enabled() || !t || !t[0]) return 0;
    char enc[512];
    backend_url_encode(enc, sizeof enc, t);
    snprintf(out, n, "%s%s%stoken=%s", backend_url(), path, strchr(path, '?') ? "&" : "?", enc);
    return 1;
}

static int rp_token_ok(const char *s) {
    if (!s || !*s || strlen(s) > 40) return 0;
    for (; *s; s++) {
        const char c = *s;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_')) return 0;
    }
    return 1;
}

// ---------------------------------------------------------------- the case cache (iîïìîíi, tok|ct)

static ReplayCase *rp_case(const char *tok, const char *ct, const int create) {
    char key[100];
    snprintf(key, sizeof key, "%s|%s", tok ? tok : "", ct ? ct : "");
    for (int i = 0; i < REPLAY_CASES; i++) {
        if (g_rp.cases[i].known && !strcmp(g_rp.cases[i].key, key)) return &g_rp.cases[i];
    }
    if (!create) return NULL;
    ReplayCase *c = &g_rp.cases[g_rp.case_next];
    g_rp.case_next = (g_rp.case_next + 1) % REPLAY_CASES;
    memset(c, 0, sizeof(*c));
    snprintf(c->key, sizeof c->key, "%s", key);
    c->known = 1;
    c->myv = -1;
    return c;
}

static int rp_num(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (int) v->valuedouble : cJSON_IsTrue(v) ? 1 : 0;
}

static void rp_case_from(ReplayCase *c, const cJSON *d) {
    c->opn = rp_num(d, "opn");
    c->rsl = rp_num(d, "rsl");
    c->ban = rp_num(d, "ban");
    c->yes = rp_num(d, "yes");
    c->no = rp_num(d, "f_no");
    const cJSON *myv = cJSON_GetObjectItemCaseSensitive(d, "myv");
    c->myv = cJSON_IsNumber(myv) ? (int) myv->valuedouble : -1;
}

// :223259 the case's status (the Demos window's): banned, resolved, the open counts, closed
static void rp_case_status(StrBuf *sb, const ReplayCase *c) {
    if (!c) return;
    if (c->ban) sb_append(sb, "<span style='color:#eb5656'>BANNED</span>");
    else if (c->rsl) sb_append(sb, "<span style='color:#fbc02d'>RESOLVED</span>");
    else if (c->opn) {
        sb_appendf(sb, "<span style='color:#7eeb7a'>%dY</span> <span style='color:#eb5656'>%dN</span>", c->yes, c->no);
        if (c->myv >= 0) sb_append(sb, " <span style='color:#fbc02d' title='Your verdict is in'>&#9679;</span>");
    } else sb_append(sb, "<span style='color:#858585'>CLOSED</span>");
}

// ---------------------------------------------------------------- the resolve area

static void rp_resolve_render(void) {
    const HtmlElem e = html_dom_by_id("replayResolve");
    if (!e) return;
    StrBuf sb = {0};
    const ReplayCase *c = rp_case(rv_token(), rv_call_token(), 0);
    if (!c || (!c->ban && !c->rsl && c->opn)) { // an open (or unknown) case: its counts, RESOLVE (:229606-229611)
        if (c && (c->yes || c->no || c->myv >= 0)) {
            rp_case_status(&sb, c);
            sb_append(&sb, " ");
        }
        sb_append(&sb, "<span class='replayResolveB' id='replayResolveBtn' onclick='replayResolve()'>VERDICT</span>");
    } else {
        rp_case_status(&sb, c);
    }
    char *html = sb_finish(&sb);
    html_dom_set_html(e, html);
    free(html);
    g_rp.resolve_mode = 0;
}

static void rp_error_done(void *user) {
    (void) user;
    g_rp.error_timer = 0;
    if (rv_active()) rp_resolve_render();
}

static void rp_resolve_error(const char *message) { // :229655-229658
    const HtmlElem e = html_dom_by_id("replayResolve");
    if (!e) return;
    StrBuf sb = {0};
    sb_append(&sb, "<span style='color:#eb5656'>");
    sb_append_esc(&sb, message && message[0] ? message : "Vote failed");
    sb_append(&sb, "</span>");
    char *html = sb_finish(&sb);
    html_dom_set_html(e, html);
    free(html);
    g_rp.resolve_mode = 3;
    if (g_rp.error_timer) html_timer_cancel(g_rp.error_timer);
    g_rp.error_timer = html_timer_add(REPLAY_VOTE_ERROR_MS, rp_error_done, NULL);
}

static int h_replay_resolve(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) { // :229613-229626
    (void) user; (void) ev; (void) args; (void) argc;
    const HtmlElem e = html_dom_by_id("replayResolve");
    if (!e || !rv_active()) return 1;
    html_dom_set_html(e, "Cheating?<span class='replayResolveB' id='replayVoteY' style='color:#eb5656' onclick='replayVote(1)'>YES</span>"
                         "<span class='replayResolveB' id='replayVoteN' style='color:#7eeb7a' onclick='replayVote(0)'>NO</span>"
                         "<span class='replayResolveB' id='replayVoteC' onclick='replayVote(-1)'>&#10005;</span>");
    g_rp.resolve_mode = 1;
    return 1;
}

static void rp_vote_done(void *user, const int status, const char *body, const size_t length) {
    const unsigned gen = (unsigned) (uintptr_t) user;
    if (gen != g_rp.gen || !demo_play_active()) return;
    cJSON *root = body && length ? cJSON_ParseWithLength(body, length) : NULL;
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (status == 200 && cJSON_IsObject(data)) { // :229634-229645
        ReplayCase *c = rp_case(rv_token(), rv_call_token(), 0);
        if (!c) { // cache[key] || {opn: 1}
            c = rp_case(rv_token(), rv_call_token(), 1);
            c->opn = 1;
        }
        c->yes = rp_num(data, "yes");
        c->no = rp_num(data, "no");
        const cJSON *myv = cJSON_GetObjectItemCaseSensitive(data, "myv");
        c->myv = cJSON_IsNumber(myv) ? (int) myv->valuedouble : c->myv;
        if (rp_num(data, "banned")) c->ban = 1;
        if (rp_num(data, "resolved")) {
            c->rsl = 1;
            c->opn = 0;
        }
        rp_resolve_render();
    } else {
        const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
        const cJSON *msg = cJSON_IsObject(error) ? cJSON_GetObjectItemCaseSensitive(error, "message") : error;
        rp_resolve_error(cJSON_IsString(msg) ? msg->valuestring : NULL);
    }
    cJSON_Delete(root);
}

static int h_replay_vote(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) { // :229628-229660
    (void) user; (void) ev;
    const int verdict = argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : -1;
    if (!demo_play_active()) return 1;
    if (verdict < 0) { // the cancel cross: back to the status (:229623)
        rp_resolve_render();
        return 1;
    }
    if (!rv_call_token()[0] || !rv_token()[0]) {
        rp_resolve_error("Open this demo from the Demos list to give a verdict");
        return 1;
    }
    char path[160], url[1024], body[160];
    snprintf(path, sizeof path, "/kpd/demos/%s/vote", rv_token());
    if (!rp_session_url(url, sizeof url, path)) {
        rp_resolve_error("Sign in to your Box Office account to vote");
        return 1;
    }
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "ct", rv_call_token());
    cJSON_AddNumberToObject(b, "verdict", verdict ? 1 : 0);
    if (!cJSON_PrintPreallocated(b, body, sizeof body, 0)) body[0] = 0;
    cJSON_Delete(b);
    const HtmlElem e = html_dom_by_id("replayResolve");
    if (e) html_dom_set_html(e, "<span style='color:#858585'>...</span>");
    g_rp.resolve_mode = 2;
    if (!backend_http_request("POST", url, body, rp_vote_done, (void *) (uintptr_t) g_rp.gen)) rp_resolve_error(NULL);
    return 1;
}

// ---------------------------------------------------------------- the bar

// :229747-229763: the segments yellow over the grey track
static void rp_paint_timeline(void) {
    const HtmlElem slider = html_dom_by_id("replaySlider");
    if (!slider) return;
    char max[16];
    snprintf(max, sizeof max, "%u", rv_duration());
    html_dom_set_attr(slider, "max", max);
    size_t count = 0;
    const DemoSegment *seg = rv_segments(&count);
    const double duration = rv_duration() ? (double) rv_duration() : 1.0;
    const char *grey = "rgba(255,255,255,0.18)", *gold = "#fbc02d";
    if (!count) {
        html_dom_set_style(slider, "background", grey);
        return;
    }
    StrBuf sb = {0};
    sb_append(&sb, "linear-gradient(to right");
    double last = 0.0;
    for (size_t i = 0; i < count; i++) {
        double a = seg[i].start / duration * 100.0, b = seg[i].end / duration * 100.0;
        a = a < 0.0 ? 0.0 : a > 100.0 ? 100.0 : a;
        b = b < a ? a : b > 100.0 ? 100.0 : b;
        sb_appendf(&sb, ",%s %.3f%%,%s %.3f%%,%s %.3f%%,%s %.3f%%", grey, last, grey, a, gold, a, gold, b);
        last = b;
    }
    sb_appendf(&sb, ",%s %.3f%%,%s 100%%)", grey, last, grey);
    char *css = sb_finish(&sb);
    html_dom_set_style(slider, "background", css);
    free(css);
}

static void rp_show(Client *client) {
    const HtmlElem bar = html_dom_by_id("replayControls");
    if (bar) html_dom_set_display(bar, "flex");
    g_rp.shown = 1;
    g_rp.last_time[0] = g_rp.last_speed[0] = g_rp.last_play[0] = g_rp.last_sus[0] = 0;
    g_rp.last_slider = UINT32_MAX;
    rp_paint_timeline();
    const HtmlElem hit = html_dom_by_id("replayHitBtn");
    if (hit) html_dom_set_style(hit, "opacity", g_rp.hitboxes ? "1" : "0.45");
    const HtmlElem sus = html_dom_by_id("replaySusBtn");
    if (sus) html_dom_set_display(sus, rv_suspect() >= 0 ? "" : "none");
    rp_resolve_render();
    (void) client;
}

static void rp_hide(void) {
    const HtmlElem bar = html_dom_by_id("replayControls");
    if (bar) html_dom_set_display(bar, "");
    g_rp.shown = 0;
    g_rp.dragging = 0;
}

// iîìîìíí (:229846-229856): follow the suspect when it is in the game; 1 when the camera is on it
static int rp_follow_suspect(Client *client) {
    const int sus = rv_suspect();
    if (sus < 0 || !client->game.player_count) return 0;
    if (spectate_target_uid() == sus) return 1;
    const Player *p = client_find_player(client, sus);
    if (!p) return 0;
    spectate_focus_player(client, sus, 1); // focusPlayer(id, false, true)
    return spectate_target_uid() == sus;
}

static int l_play(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) ev; (void) args; (void) argc;
    demo_play_set_playing(!demo_play_playing());
    return 1;
}

static int l_slider_down(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) ev; (void) args; (void) argc;
    g_rp.dragging = 1;
    return 1;
}

static int l_slider_change(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) args; (void) argc;
    g_rp.dragging = 0;
    if (!g_rp.client || !ev || !ev->value) return 1;
    demo_play_seek(g_rp.client, (uint32_t) strtoul(ev->value, NULL, 10));
    return 1;
}

static int l_speed(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) ev; (void) args; (void) argc;
    demo_play_cycle_speed();
    return 1;
}

static int l_suspect(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) ev; (void) args; (void) argc;
    if (g_rp.client) rp_follow_suspect(g_rp.client);
    return 1;
}

static int l_hitboxes(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) ev; (void) args; (void) argc;
    g_rp.hitboxes = !g_rp.hitboxes;
    const HtmlElem hit = html_dom_by_id("replayHitBtn");
    if (hit) html_dom_set_style(hit, "opacity", g_rp.hitboxes ? "1" : "0.45");
    return 1;
}

static int l_exit(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) ev; (void) args; (void) argc;
    if (!g_rp.client) return 1;
    demo_play_stop(g_rp.client);
    rp_hide();
    client_set_menu_open(g_rp.client, 1);
    return 1;
}

void hud_replay_bind(Client *client) {
    g_rp.client = client;
    if (g_rp.bound) return;
    g_rp.bound = 1;
    html_ui_set_listener("replayPlayBtn", "click", l_play);
    html_ui_set_listener("replaySlider", "mousedown", l_slider_down);
    html_ui_set_listener("replaySlider", "change", l_slider_change);
    html_ui_set_listener("replaySpeed", "click", l_speed);
    html_ui_set_listener("replaySusBtn", "click", l_suspect);
    html_ui_set_listener("replayHitBtn", "click", l_hitboxes);
    html_ui_set_listener("replayExitBtn", "click", l_exit);
    html_ui_add_handler("replayResolve", h_replay_resolve);
    html_ui_add_handler("replayVote", h_replay_vote);
}

void hud_replay_tick(Client *client) {
    if (!client) return;
    g_rp.client = client;
    if (g_sc.on) { // --render-scenario: the fixed bar (hud_replay_scenario)
        if (!g_rp.shown) rp_show(client);
    } else if (!demo_play_active()) {
        if (g_rp.shown) rp_hide();
        return;
    }
    if (!g_rp.bound) hud_replay_bind(client);
    if (!g_sc.on && demo_play_take_round_start()) { // onRoundStart (:229724-229731)
        ui_loadout_clear_pops();
        client_set_menu_open(client, 0);
        if (!rp_follow_suspect(client)) spectate_mode(client, 1);
        g_rp.refollow_at = glfwGetTime() + REPLAY_REFOLLOW_S;
        if (!g_rp.shown) rp_show(client);
    }
    if (!g_rp.shown) return;
    demo_play_take_ended();

    char text[48], a[16], b[16];
    rp_time_text(a, sizeof a, rv_playhead());
    rp_time_text(b, sizeof b, rv_duration());
    snprintf(text, sizeof text, "%s / %s", a, b);
    rp_set_text("replayTime", text, g_rp.last_time, sizeof g_rp.last_time);
    const float speed = rv_speed();
    snprintf(text, sizeof text, speed < 1.0f ? "%.1fx" : "%.0fx", speed);
    rp_set_text("replaySpeed", text, g_rp.last_speed, sizeof g_rp.last_speed);
    rp_set_text("replayPlayBtn", rv_playing() ? "pause" : "play_arrow", g_rp.last_play, sizeof g_rp.last_play);
    if (rv_suspect() >= 0) {
        snprintf(text, sizeof text, "Suspect: Player_%d", rv_suspect());
        rp_set_text("replaySusBtn", text, g_rp.last_sus, sizeof g_rp.last_sus);
    }
    const uint32_t at = rv_playhead();
    if (!g_rp.dragging && at != g_rp.last_slider) {
        const HtmlElem slider = html_dom_by_id("replaySlider");
        snprintf(text, sizeof text, "%u", at);
        if (slider) html_dom_set_value(slider, text);
        g_rp.last_slider = at;
    }
    // the 1.5 s re-follow while playing (:229810-229812)
    const double now = glfwGetTime();
    if (now >= g_rp.refollow_at) {
        g_rp.refollow_at = now + REPLAY_REFOLLOW_S;
        if (!g_sc.on && demo_play_playing() && spectate_target_uid() != rv_suspect()) rp_follow_suspect(client);
    }
}

// ---------------------------------------------------------------- watchDemo / openDemoLink

static void rp_download_done(void *user, const int status, const char *body, const size_t length) {
    const unsigned gen = (unsigned) (uintptr_t) user;
    if (gen != g_rp.gen || !g_rp.client) return;
    if (status != 200 || !body || !length) {
        fprintf(stderr, "[replay] demo %s: download failed (HTTP %d)\n", g_rp.tok, status);
        ui_loadout_response_popup("Demo Download Failed"); // :229815
        return;
    }
    char err[64] = "";
    if (!demo_play_start(g_rp.client, (const uint8_t *) body, length, g_rp.tok, g_rp.ct, err, sizeof err)) {
        ui_loadout_response_popup(err[0] ? err : "Invalid Demo File"); // :229714
        return;
    }
    ui_loading_popup("Loading replay..."); // :229723, closed at onRoundStart
    menu_close_window();
}

void hud_replay_watch(Client *client, const char *tok, const char *ct) {
    if (client) g_rp.client = client;
    client = g_rp.client;
    if (!client || !rp_token_ok(tok) || (ct && ct[0] && !rp_token_ok(ct))) {
        ui_loadout_response_popup("Demo Download Failed");
        return;
    }
    snprintf(g_rp.tok, sizeof g_rp.tok, "%s", tok);
    snprintf(g_rp.ct, sizeof g_rp.ct, "%s", ct ? ct : "");
    g_rp.gen++;
    char path[128];
    snprintf(path, sizeof path, "/kpd/demos/%s/download", tok);
    ui_loading_popup("Downloading demo..."); // :229697
    if (!backend_http_get_auth(path, account_session_token(), rp_download_done, (void *) (uintptr_t) g_rp.gen))
        ui_loadout_response_popup("Demo Download Failed");
}

// The case popup (:229824-229829) from demoInfo's data; 0 when its tokens are not a demo's
static int rp_case_popup(const cJSON *d) {
    const cJSON *tok = cJSON_GetObjectItemCaseSensitive(d, "tok"), *ct = cJSON_GetObjectItemCaseSensitive(d, "ct");
    const char *t = cJSON_IsString(tok) ? tok->valuestring : g_rp.tok, *c = cJSON_IsString(ct) ? ct->valuestring : g_rp.ct;
    if (!rp_token_ok(t) || (c[0] && !rp_token_ok(c))) return 0;
    ReplayCase *rc = rp_case(t, c, 1);
    rp_case_from(rc, d);

    const int typ = rp_num(d, "typ");
    const cJSON *map = cJSON_GetObjectItemCaseSensitive(d, "map"), *mode = cJSON_GetObjectItemCaseSensitive(d, "mod");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(d, "name");
    char date[32] = "?", dur[16];
    const cJSON *dat = cJSON_GetObjectItemCaseSensitive(d, "dat");
    if (cJSON_IsNumber(dat) && dat->valuedouble > 0) {
        const time_t when = (time_t) dat->valuedouble;
        struct tm tm;
#ifdef _WIN32
        if (!localtime_s(&tm, &when))
#else
        if (localtime_r(&when, &tm))
#endif
            strftime(date, sizeof date, "%Y-%m-%d %H:%M", &tm);
    }
    rp_time_text(dur, sizeof dur, (uint32_t) (rp_num(d, "dur") > 0 ? rp_num(d, "dur") : 0));

    StrBuf sb = {0};
    sb_append(&sb, "<div style='font-size:26px;margin-bottom:4px;color:#858585'>K.P.D Case Review</div>"
                   "<div style='line-height:1.8;margin-bottom:12px'><div style='font-size:19px'>");
    sb_append(&sb, typ == 1 ? "[ranked] " : typ == 2 ? "[comp] " : typ == 3 ? "[custom] " : "");
    sb_append_esc(&sb, cJSON_IsString(map) && map->valuestring[0] ? map->valuestring : "?");
    sb_append(&sb, "</div><div style='color:#858585'>");
    sb_append_esc(&sb, cJSON_IsString(mode) && mode->valuestring[0] ? mode->valuestring : "?");
    sb_appendf(&sb, "</div><div style='color:#858585'>%s &middot; %s</div></div>", date, dur);
    sb_append(&sb, "<div style='line-height:1.8;margin-bottom:8px'><span style='font-size:17px'>");
    if (rc->ban && cJSON_IsString(name) && name->valuestring[0]) {
        sb_append(&sb, "<span style='color:var(--red)'>");
        sb_append_esc(&sb, name->valuestring);
        sb_append(&sb, "</span>");
    } else {
        sb_append(&sb, "Suspect");
        if (rp_num(d, "gst")) sb_append(&sb, " <span style='color:#858585;font-size:13px'>guest</span>");
    }
    sb_append(&sb, "</span><span style='margin:0px 10px'>");
    rp_case_status(&sb, rc);
    sb_appendf(&sb, "</span></div><div style='white-space:nowrap'><div id='confirmBtn' style='width:142px;display:inline-block;vertical-align:middle' "
                    "onclick=\"watchDemo('%s','%s')\">Watch</div><span class='material-icons' style='font-size:20px;vertical-align:middle;cursor:pointer;"
                    "color:#fff;margin-left:10px' title='Copy the case link' onclick=\"copyDemoLink(this,'%s','%s')\">content_copy</span></div>",
               t, c, t, c);
    char *html = sb_finish(&sb);
    ui_loadout_clear_pops();
    v10_popup_show("polConfPop", html, NULL);
    free(html);
    return 1;
}

static void rp_info_done(void *user, const int status, const char *body, const size_t length) {
    const unsigned gen = (unsigned) (uintptr_t) user;
    if (gen != g_rp.gen) return;
    cJSON *root = body && length ? cJSON_ParseWithLength(body, length) : NULL;
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(root, "data");
    // :229821-229823, :229831
    if (status != 200 || !cJSON_IsObject(d) || !rp_case_popup(d)) ui_loadout_response_popup("Demo not found, or not yours to review");
    cJSON_Delete(root);
}

void hud_replay_open_link(Client *client, const char *tok, const char *ct) {
    if (client) g_rp.client = client;
    if (!rp_token_ok(tok) || (ct && ct[0] && !rp_token_ok(ct))) {
        ui_loadout_response_popup("Demo not found, or not yours to review");
        return;
    }
    snprintf(g_rp.tok, sizeof g_rp.tok, "%s", tok);
    snprintf(g_rp.ct, sizeof g_rp.ct, "%s", ct ? ct : "");
    g_rp.gen++;
    char path[160];
    snprintf(path, sizeof path, "/kpd/demos/%s?ct=%s", tok, ct ? ct : "");
    ui_loading_popup("Loading..."); // :229819
    if (!backend_http_get_auth(path, account_session_token(), rp_info_done, (void *) (uintptr_t) g_rp.gen))
        ui_loadout_response_popup("Demo not found, or not yours to review");
}

// ---------------------------------------------------------------- --render-scenario

void hud_replay_scenario(Client *client, const cJSON *bar, const cJSON *case_popup) {
    if (cJSON_IsObject(case_popup)) rp_case_popup(case_popup);
    if (!cJSON_IsObject(bar)) return;
    g_rp.client = client;
    memset(&g_sc, 0, sizeof(g_sc));
    g_sc.on = 1;
    g_sc.t = (uint32_t) rp_num(bar, "time");
    g_sc.duration = (uint32_t) rp_num(bar, "duration");
    const cJSON *speed = cJSON_GetObjectItemCaseSensitive(bar, "speed");
    g_sc.speed = cJSON_IsNumber(speed) ? (float) speed->valuedouble : 1.0f;
    const cJSON *playing = cJSON_GetObjectItemCaseSensitive(bar, "playing");
    g_sc.playing = playing ? cJSON_IsTrue(playing) || rp_num(bar, "playing") : 1;
    const cJSON *suspect = cJSON_GetObjectItemCaseSensitive(bar, "suspect");
    g_sc.suspect = cJSON_IsNumber(suspect) ? suspect->valueint : -1;
    const cJSON *seg;
    cJSON_ArrayForEach(seg, cJSON_GetObjectItemCaseSensitive(bar, "segments")) {
        if (g_sc.segment_count >= DEMO_SEGMENTS_MAX || cJSON_GetArraySize(seg) != 2) continue;
        g_sc.segments[g_sc.segment_count].start = (uint32_t) cJSON_GetArrayItem(seg, 0)->valuedouble;
        g_sc.segments[g_sc.segment_count++].end = (uint32_t) cJSON_GetArrayItem(seg, 1)->valuedouble;
    }
    const cJSON *tok = cJSON_GetObjectItemCaseSensitive(bar, "tok"), *ct = cJSON_GetObjectItemCaseSensitive(bar, "ct");
    snprintf(g_sc.tok, sizeof g_sc.tok, "%s", cJSON_IsString(tok) ? tok->valuestring : "");
    snprintf(g_sc.ct, sizeof g_sc.ct, "%s", cJSON_IsString(ct) ? ct->valuestring : "");
    g_rp.hitboxes = rp_num(bar, "hitboxes");
    const cJSON *kase = cJSON_GetObjectItemCaseSensitive(bar, "case");
    if (cJSON_IsObject(kase)) rp_case_from(rp_case(g_sc.tok, g_sc.ct, 1), kase);
    rp_show(client);
    hud_replay_tick(client);
    const cJSON *resolve = cJSON_GetObjectItemCaseSensitive(bar, "resolve");
    if (cJSON_IsString(resolve) && !strcmp(resolve->valuestring, "question")) h_replay_resolve(NULL, NULL, NULL, 0);
    else if (cJSON_IsString(resolve) && !strcmp(resolve->valuestring, "error")) rp_resolve_error(NULL);
}
