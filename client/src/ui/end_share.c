// The end-of-match table's actions column (Krunker v10.0.7): your own row's share button and another account's
// report button (deob-10.0.7.js:226776-226777), the share popup with its note box (#shareTxt, :227048-227054) and
// its send (:227055-227064: SHARE_MATCH_RESULT "smr" with the note, then the popups close).
// The share goes to the account's backend: the official game socket for a krunker.io account, the social socket for
// a Box Office one (services/brain: the match is posted to the player's feed). The wording is Box Office's own.
#include <end_share.h>

#include <account_split.h>
#include <backend.h>
#include <backend_ui.h>
#include <html_dom.h>
#include <html_ui.h>
#include <i18n.h>
#include <krunker_net.h>
#include <ui_logic.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHARE_NOTE_MAX 280

static void escape_attr(char *out, const size_t n, const char *s) {
    size_t k = 0;
    for (; s && *s && k + 7 < n; s++) {
        const char *r = *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '&' ? "&amp;" : *s == '"' ? "&quot;" : *s == '\'' ? "&#39;" : *s == '`' ? "&#96;" : NULL;
        if (r) {
            const size_t l = strlen(r);
            memcpy(out + k, r, l);
            k += l;
        } else {
            out[k++] = *s;
        }
    }
    out[k] = 0;
}

void end_share_actions_html(char *out, const size_t size, const int is_you, const int other_has_account, const int custom_game,
                            const char *name) {
    out[0] = 0;
    // a guest gets neither button (:226777 `account && ...`); a --render-scenario stand-in login counts
    if (!account_split_signed_in() && !account_ui_scenario_signed_in()) return;
    if (is_you) {
        if (custom_game) return; // no share for a custom game
        snprintf(out, size, "<div class='material-icons reportBut shareBut' title='%s' onclick='shareResult()'>share</div>",
                 I18N("kn.share.button", "Share this result"));
        return;
    }
    if (!other_has_account || !name || !*name) return;
    char safe[128];
    escape_attr(safe, sizeof safe, name);
    snprintf(out, size, "<div class='material-icons reportBut' title='%s' onclick='reportPlayer(\"%s\")'>flag</div>",
             I18N("kn.share.report", "Report this player"), safe);
}

static void share_popup(void) {
    ui_loadout_reset_pops(); // resetPops: #popupHolder up, every other popup hidden
    const HtmlElem pop = html_dom_by_id("genericPop");
    if (!pop) return;
    html_dom_set_attr(pop, "class", "kn-share-pop");
    html_dom_set_attr(pop, "style", "");
    char html[1024];
    snprintf(html, sizeof html,
             "<div style='color:#fff;font-size:20px;margin-bottom:15px'>%s</div>"
             "<textarea id='shareTxt' rows='5' maxlength='%d' style='width:100%%' placeholder='%s'></textarea>"
             "<div id='confirmBtn' style='width:calc(100%% - 20px)' onmouseover='playTick(0.1)' onclick='shareMatch()'>%s</div>",
             I18N("kn.share.title", "Share this match"), SHARE_NOTE_MAX, I18N("kn.share.placeholder", "Add a note to your results (optional)"),
             I18N("kn.share.send", "Post"));
    html_dom_set_html(pop, html);
    html_dom_set_display(pop, "block");
}

static void share_send(void) {
    char note[SHARE_NOTE_MAX + 1] = "";
    const HtmlElem box = html_dom_by_id("shareTxt");
    if (box) html_dom_get_value(box, note, sizeof note);
    cJSON *frame = cJSON_CreateArray();
    cJSON_AddItemToArray(frame, cJSON_CreateString("smr"));
    cJSON_AddItemToArray(frame, cJSON_CreateString(note));
    if (account_backend() == ACCOUNT_BACKEND_KRUNKER) krunker_net_send_account_frame(frame);
    else backend_social_send(frame);
    cJSON_Delete(frame);
    ui_loadout_clear_pops(); // clearPops()
}

int end_share_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    if (!name) return -1;
    if (!strcmp(name, "reportPlayer")) { // reportPlayer(name): the player's profile (:227066-227070)
        if (argc > 0 && args[0].kind == HTML_ARG_STR && args[0].str && *args[0].str) {
            const HtmlArg a = {.kind = HTML_ARG_STR, .str = args[0].str};
            html_ui_dispatch("openPlayerProfile", NULL, &a, 1);
        }
        return 1;
    }
    if (!strcmp(name, "shareResult")) {
        menu_request_open(1); // toggleMenu(true)
        share_popup();
        return 1;
    }
    if (!strcmp(name, "shareMatch")) {
        share_send();
        return 1;
    }
    return -1;
}
