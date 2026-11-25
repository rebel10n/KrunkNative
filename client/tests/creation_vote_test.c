// creation_vote_test: the menu's map / mod vote (client/src/ui/creation_vote.c) against stubbed DOM, brain and account.
#include <creation_vote.h>

#include <backend_ui.h>
#include <i18n.h>
#include <ui_logic.h>
#include <v10_social.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); g_fail = 1; } } while (0)

// ---- stubs: a tiny id -> {html, display, removed} table
typedef struct { const char *id; char html[1024]; char display[16]; int removed; } El;
// modVoteU's parent stands for the thumbs' box of #modVoteHold's markup (v10 gives it no id)
static El g_els[] = {{"mapVote"}, {"modVote"}, {"modVoteName"}, {"modVoteHold"}, {"mapVoteU"}, {"mapVoteD"}, {"(modVoteU's parent)"}, {"modVoteU"}};
#define NELS ((int) (sizeof(g_els) / sizeof(g_els[0])))
HtmlElem html_dom_by_id(const char *id) {
    for (int i = 0; i < NELS; i++)
        if (!strcmp(g_els[i].id, id) && !g_els[i].removed) return (HtmlElem) (i + 1);
    return 0;
}
void html_dom_set_html(HtmlElem e, const char *html) { snprintf(g_els[e - 1].html, sizeof(g_els[0].html), "%s", html); }
void html_dom_set_text(HtmlElem e, const char *t) { snprintf(g_els[e - 1].html, sizeof(g_els[0].html), "%s", t); }
void html_dom_set_display(HtmlElem e, const char *v) { snprintf(g_els[e - 1].display, sizeof(g_els[0].display), "%s", v); }
void html_dom_remove(HtmlElem e) { g_els[e - 1].removed = 1; }
HtmlElem html_dom_parent(HtmlElem e) { return e == 8 ? (HtmlElem) 7 : 0; }
const char *i18n_tr(const char *key, const char *english) { (void) key; return english; }

static char g_notice[256];
void ui_notify(const char *html) { snprintf(g_notice, sizeof(g_notice), "%s", html); }

static char g_path[64];
static int g_sent_vote = 99, g_sends;
int v10_gapi(const char *method, const char *path, const cJSON *body, V10GapiFn fn, void *user) {
    (void) fn; (void) user;
    g_sends++;
    snprintf(g_path, sizeof(g_path), "%s %s", method, path);
    g_sent_vote = (int) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(body, "vote"));
    return 1;
}
const char *v10_gapi_error_text(const cJSON *error, const char *fallback) { (void) error; return fallback; }
static AccountInfo g_acc;
static int g_signed_in = 1;
const AccountInfo *active_account(void) { return g_signed_in ? &g_acc : NULL; }

static HtmlArg num(double v) { return (HtmlArg) {HTML_ARG_NUM, v, NULL}; }
static int press(const char *fn, int id, int v) {
    HtmlArg a[2] = {num(id), num(v)};
    return creation_vote_handle(fn, NULL, a, 2);
}
static void reset_els(void) {
    for (int i = 0; i < NELS; i++) { g_els[i].html[0] = 0; g_els[i].display[0] = 0; g_els[i].removed = 0; }
}

int main(void) {
    // the toggle (deob-10.0.7.js:227009, :227020): same side again clears it, the other side sets it
    CHECK(creation_vote_next(0, 1) == 1 && creation_vote_next(1, 1) == 0 && creation_vote_next(1, -1) == -1, "up toggles");
    CHECK(creation_vote_next(0, -1) == -1 && creation_vote_next(-1, -1) == 0 && creation_vote_next(-1, 1) == 1, "down toggles");

    // a hosted map fills #mapVote with both buttons
    creation_vote_set_map(4242, "Arena", "alice");
    CHECK(strstr(g_els[0].html, "id='mapVoteU'") && strstr(g_els[0].html, "id='mapVoteD'"), "map buttons rendered");
    // Donate opens the creator's profile with autoGift 2 and the map's name (deob-10.0.7.js:226239)
    CHECK(strstr(g_els[0].html, "voteMap(4242,1)") && strstr(g_els[0].html, "openPlayerProfile(\"alice\",2,\"Arena\")"), "map handlers");
    // v10's label and markup: a voteHint div "Donate", the thumbs in a float:right div (:226239)
    CHECK(strstr(g_els[0].html, "<div class='voteHint'  onclick='openPlayerProfile(\"alice\",2,\"Arena\")'>Donate</div>"
                                "<div style='float:right;'><i id='mapVoteD' onclick='voteMap(4242,-1)' class='material-icons vote'>thumb_down</i>"
                                "<i id='mapVoteU' onclick='voteMap(4242,1)' class='material-icons vote'>thumb_up</i></div>") != NULL,
          "map panel: v10's Donate link and thumbs markup");
    CHECK(!strstr(g_els[0].html, "Tip the creator"), "map panel: no Box Office tip wording");

    // a creator name cannot break out of the inline handler
    creation_vote_set_map(4242, "Arena", "al\"ice');x()//");
    CHECK(!strstr(g_els[0].html, "x()") || !strstr(g_els[0].html, "');x"), "creator is sanitised");
    CHECK(strstr(g_els[0].html, "openPlayerProfile(\"alicex\",2,\"Arena\")") != NULL, "creator keeps its safe characters");

    // an upvote posts to the brain and removes the buttons. The panel's onclick passes no third
    // argument, so v10 draws no "Map Upvoted" line (:227019, :227027).
    g_sends = 0;
    g_notice[0] = 0;
    press("voteMap", 4242, 1);
    CHECK(g_sends == 1 && !strcmp(g_path, "POST /maps/4242/vote") && g_sent_vote == 1, "map upvote sent");
    CHECK(g_els[4].removed && g_els[5].removed, "map buttons removed after voting");
    CHECK(g_notice[0] == 0, "map upvote is silent");

    // pressing up again (after the next INIT re-renders) takes it back
    reset_els();
    creation_vote_set_map(4242, "Arena", "alice");
    press("voteMap", 4242, 1);
    CHECK(g_sent_vote == 0, "second upvote clears the vote");

    // a different map starts fresh
    reset_els();
    creation_vote_set_map(7, "Other", "bob");
    press("voteMap", 7, -1);
    CHECK(!strcmp(g_path, "POST /maps/7/vote") && g_sent_vote == -1, "new map: downvote");

    // an id that is not the shown map is ignored
    g_sends = 0;
    press("voteMap", 999, 1);
    CHECK(g_sends == 0, "stale id ignored");

    // an official map (id 0) empties the panel
    creation_vote_set_map(0, "", "");
    CHECK(g_els[0].html[0] == 0 && creation_vote_map_id() == 0, "official map: no vote");

    // a guest's press sends nothing and empties the panel (:227031)
    reset_els();
    creation_vote_set_map(4242, "Arena", "alice");
    g_signed_in = 0;
    g_sends = 0;
    press("voteMap", 4242, 1);
    CHECK(g_sends == 0 && g_els[0].html[0] == 0, "guest: no vote, panel emptied");
    g_signed_in = 1;

    // a loaded mod shows #modVote; its vote posts to /mods/:id/vote and hides the thumbs, not Donate
    reset_els();
    creation_vote_set_mod(55, "Neon", "carol");
    CHECK(!strcmp(g_els[1].display, "block") && !strcmp(g_els[2].html, "Neon Mod"), "mod panel shown as \"<name> Mod\" (:225875)");
    CHECK(strstr(g_els[3].html, "voteMod(55, -1)") && strstr(g_els[3].html, "id=\"modVoteU\""), "mod buttons rendered");
    CHECK(strstr(g_els[3].html, "openPlayerProfile(\"carol\",1,\"Neon\")") != NULL, "mod Donate: autoGift 1 and the mod's name (deob-10.0.7.js:225878)");
    CHECK(strstr(g_els[3].html, "<div class=\"voteHint vHR\" onclick='openPlayerProfile(\"carol\",1,\"Neon\")'>Donate</div>\n"
                                "\t\t\t\t<div style=\"float:right;\">\n"
                                "\t\t\t\t\t<i id=\"modVoteD\" onclick=\"voteMod(55, -1);\" class='material-icons vote'>thumb_down</i>") != NULL,
          "mod panel: v10's Donate link and thumbs markup (:225878)");
    press("voteMod", 55, -1);
    CHECK(!strcmp(g_path, "POST /mods/55/vote") && g_sent_vote == -1 && !strcmp(g_els[6].display, "none") && g_els[3].display[0] == 0,
          "mod downvote hides the thumbs and leaves Donate");
    creation_vote_set_mod(0, NULL, NULL);
    CHECK(!strcmp(g_els[1].display, "none"), "no mod: panel hidden");

    // names that are not ours fall through
    CHECK(creation_vote_handle("openSocial", NULL, NULL, 0) == -1, "other names are not handled");

    if (!g_fail) printf("creation_vote_test: OK\n");
    return g_fail;
}
