// The menu's vote on the community map this game hosts (#mapVote) and on the player's loaded mod (#modVote).
//
// Behaviour follows Krunker v10.0.7; the markup and wording are Box Office's own:
//   - a hosted published map fills #mapVote with a tip link for its creator and thumbs down / up
//     (deob-10.0.7.js:226239, from INIT's custom map id and creator; here NET_EXT_KN_CREATION_INFO);
//     an official map or a map the brain does not know leaves it empty (:226235);
//   - voteMap(id, v) (:227018-227031): pressing the side already chosen clears the vote (0), the other
//     side sets it (1 / -1); the choice goes to the server once and the two buttons are removed. The
//     "Map Upvoted" line runs only when a third argument is passed, and the panel's onclick does not
//     pass one, so a vote from the panel is silent. A guest's press just empties the panel (:227031);
//   - voteMod(id, v) (:227008-227016): the same three-way choice for the loaded mod, then the thumbs'
//     box is hidden and the Donate link stays; #modVote shows while a mod with a brain id is loaded
//     (:225872-225880).
// Krunker sends the vote on the game socket (MAP_VOTE "vote"); Box Office's maps and mods live in kn_brain,
// so the client posts it there (services/brain/v10_catalog.c POST /maps/:id/vote, v10_mods.c POST
// /mods/:id/vote), which keeps one vote per account and the net count.
#include <creation_vote.h>

#include <backend_ui.h>
#include <html_dom.h>
#include <i18n.h>
#include <ui_logic.h>
#include <v10_social.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int id;            // the brain's id, 0 = none
    char name[64];
    char creator[32];
    int vote;          // this session's choice: -1, 0, 1
} CreationVote;

static CreationVote g_map, g_mod;

// HTML-escape into out (names come from players)
static void esc(char *out, const size_t size, const char *in) {
    size_t o = 0;
    for (const char *p = in ? in : ""; *p && o + 7 < size; p++) {
        const char *r = NULL;
        switch (*p) {
            case '&': r = "&amp;"; break;
            case '<': r = "&lt;"; break;
            case '>': r = "&gt;"; break;
            case '"': r = "&quot;"; break;
            case '\'': r = "&#39;"; break;
            default: break;
        }
        if (r) {
            const size_t n = strlen(r);
            memcpy(out + o, r, n);
            o += n;
        } else {
            out[o++] = *p;
        }
    }
    out[o] = 0;
}

// An inline handler's string argument: letters, digits, space, - _ . only (account and map names), so the
// value can sit inside onclick='...("...")' without escaping rules of its own.
static void js_arg(char *out, const size_t size, const char *in) {
    size_t o = 0;
    for (const char *p = in ? in : ""; *p && o + 1 < size; p++) {
        const unsigned char c = (unsigned char) *p;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == ' ' || c == '-' || c == '_' || c == '.')
            out[o++] = (char) c;
    }
    out[o] = 0;
}

// The next choice after pressing side `press` (1 up, -1 down) with `current` already chosen (:227020, :227009).
int creation_vote_next(const int current, const int press) {
    if (press == 1) return current == 1 ? 0 : 1;
    if (press == -1) return current == -1 ? 0 : -1;
    return 0;
}

static void render_map(void) {
    const HtmlElem box = html_dom_by_id("mapVote");
    if (!box) return;
    if (!g_map.id) {
        html_dom_set_html(box, "");
        return;
    }
    char who[48], what[160], label[80], html[800];
    js_arg(who, sizeof(who), g_map.creator);
    js_arg(what, sizeof(what), g_map.name);
    esc(label, sizeof(label), I18N("kn.creationVote.tip", "Tip the creator"));
    // openPlayerProfile(creator, {autoGift: 2, assetName: map}) (:226239): the gift view says "I like your <map> map"
    snprintf(html, sizeof(html),
             "<span class=\"voteHint\" onclick='openPlayerProfile(\"%s\",2,\"%s\")'>%s</span>"
             "<span class=\"kn-vote-pair\">"
             "<i id=\"mapVoteD\" class=\"material-icons vote\" onclick=\"voteMap(%d,-1)\">thumb_down</i>"
             "<i id=\"mapVoteU\" class=\"material-icons vote\" onclick=\"voteMap(%d,1)\">thumb_up</i>"
             "</span>",
             who, what, label, g_map.id, g_map.id);
    html_dom_set_html(box, html);
}

static void render_mod(void) {
    const HtmlElem box = html_dom_by_id("modVote");
    if (!box) return;
    if (!g_mod.id) {
        html_dom_set_display(box, "none");
        return;
    }
    char name[96], text[128];
    esc(name, sizeof(name), g_mod.name[0] ? g_mod.name : "Mod");
    const HtmlElem title = html_dom_by_id("modVoteName");
    if (title) {
        snprintf(text, sizeof(text), "%s", g_mod.name[0] ? g_mod.name : "Mod");
        html_dom_set_text(title, text);
    }
    const HtmlElem hold = html_dom_by_id("modVoteHold");
    if (hold) {
        char who[48], what[160], label[80], html[800];
        js_arg(who, sizeof(who), g_mod.creator);
        js_arg(what, sizeof(what), g_mod.name);
        esc(label, sizeof(label), I18N("kn.creationVote.tip", "Tip the creator"));
        // openPlayerProfile(creator, {autoGift: 1, assetName: mod}) (:225878): "I like your <mod> mod"
        snprintf(html, sizeof(html),
                 "<span class=\"voteHint vHR\" onclick='openPlayerProfile(\"%s\",1,\"%s\")'>%s</span>"
                 "<span class=\"kn-vote-pair\" id=\"modVotePair\">"
                 "<i id=\"modVoteD\" class=\"material-icons vote\" onclick=\"voteMod(%d,-1)\">thumb_down</i>"
                 "<i id=\"modVoteU\" class=\"material-icons vote\" onclick=\"voteMod(%d,1)\">thumb_up</i>"
                 "</span>",
                 who, what, label, g_mod.id, g_mod.id);
        html_dom_set_html(hold, html);
        html_dom_set_display(hold, "");
    }
    html_dom_set_display(box, "block");
}

void creation_vote_set_map(const int map_id, const char *name, const char *creator) {
    if (map_id != g_map.id) g_map.vote = 0; // :226236: a new map starts with no choice
    g_map.id = map_id > 0 ? map_id : 0;
    snprintf(g_map.name, sizeof(g_map.name), "%s", name ? name : "");
    snprintf(g_map.creator, sizeof(g_map.creator), "%s", creator ? creator : "");
    render_map();
}

void creation_vote_set_mod(const int mod_id, const char *name, const char *creator) {
    if (mod_id != g_mod.id) g_mod.vote = 0; // :225873
    g_mod.id = mod_id > 0 ? mod_id : 0;
    snprintf(g_mod.name, sizeof(g_mod.name), "%s", name ? name : "");
    snprintf(g_mod.creator, sizeof(g_mod.creator), "%s", creator ? creator : "");
    render_mod();
}

static void vote_done(void *user, const int status, const cJSON *data, const cJSON *error) {
    (void) data;
    const int is_mod = (int) (intptr_t) user;
    if (status >= 200 && status < 300 && !error) return;
    char text[256];
    esc(text, sizeof(text), v10_gapi_error_text(error, is_mod ? "Your mod vote could not be saved" : "Your map vote could not be saved"));
    ui_notify(text);
}

static void send_vote(const int is_mod, const int id, const int vote) {
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "vote", vote);
    char path[48];
    snprintf(path, sizeof(path), is_mod ? "/mods/%d/vote" : "/maps/%d/vote", id);
    if (!v10_gapi("POST", path, body, vote_done, (void *) (intptr_t) is_mod)) vote_done((void *) (intptr_t) is_mod, 0, NULL, NULL);
    cJSON_Delete(body);
}

static void remove_by_id(const char *id) {
    const HtmlElem el = html_dom_by_id(id);
    if (el) html_dom_remove(el);
}

int creation_vote_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    if (!name) return -1;
    const int is_map = !strcmp(name, "voteMap"), is_mod = !strcmp(name, "voteMod");
    if (!is_map && !is_mod) return -1;
    const int id = argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : 0;
    const int press = argc > 1 && args[1].kind == HTML_ARG_NUM ? (int) args[1].num : 0;
    CreationVote *v = is_map ? &g_map : &g_mod;
    if (!id || id != v->id) return 0;
    if (!active_account()) { // :227031: no account, no vote; the panel empties
        if (is_map) {
            const HtmlElem box = html_dom_by_id("mapVote");
            if (box) html_dom_set_html(box, "");
        }
        return 0;
    }
    v->vote = creation_vote_next(v->vote, press);
    send_vote(is_mod, id, v->vote);
    if (is_map) { // the panel's onclick passes no third argument, so there is no "Map Upvoted" line
        remove_by_id("mapVoteD");
        remove_by_id("mapVoteU");
    } else {
        // the thumbs' box (modVoteU's parent), not the Donate link beside it (:227016)
        const HtmlElem pair = html_dom_by_id("modVotePair");
        if (pair) html_dom_set_display(pair, "none");
    }
    return 0;
}

int creation_vote_map_id(void) { return g_map.id; }
int creation_vote_mod_id(void) { return g_mod.id; }
