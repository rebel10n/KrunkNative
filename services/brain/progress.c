/*
 * services/brain/progress.c - player-viewer WP-E part 2c: the menu's badge, challenge and medal
 * requests on the social socket (doc/backend.md 7.4). v7.1.4 sends "bdg" / "cbdg" / "chg" on
 * the game socket (index.js selectBadge / claimBadge, windows[32]); KrunkNative's menu talks to
 * kn_brain directly, like the rest of its account requests. The database work is
 * db_progress.c; the match-end pushes ("chg", "chgC", "chlR", "nbdgr") come from the stats
 * commit (accounts.c progress_done).
 *
 *   ["chg"]          -> ["chgL", activeChallenges]   (KrunkNative label: v7's "chg" is the
 *                                                     match-end result triples)
 *   ["bdg", badge]   -> ["bdgr", badge] | ["po", null, err]     (io_badgeUpdate)
 *   ["flg", index]   -> ["flgr", index] | ["po", null, err]     (io_flgr; the Country Flag, stats.flg)
 *   ["cbdg", badge]  -> ["cbdgr", badge] | ["po", null, err]    (io_badgeClaimResponse)
 *   ["mdl"]          -> ["mdl", {medal: count}]      (KrunkNative: the account's medal counts)
 */
#include "acct_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "db_procs.h"
#include "knsvc/json.h"

typedef struct {
    char label[8];
} ProgReply;

static ProgReply *prog_reply(const char *label) {
    ProgReply *r = calloc(1, sizeof *r);
    if (r) snprintf(r->label, sizeof r->label, "%s", label);
    return r;
}

/* One JSON column -> [label, value | field]; a refusal -> ["po", null, msg] (io_po popup). */
static void prog_done(SocialSession *s, kn_db_result *res, void *extra) {
    ProgReply *r = extra;
    if (!kn_db_ok(res)) {
        social_error(s, acct_db_error(res, r->label));
        return;
    }
    cJSON *v = acct_db_json(res);
    cJSON *out = NULL;
    if (!strcmp(r->label, "bdgr")) out = cJSON_CreateNumber((double) kn_json_int(v, "bdg", -1));
    else if (!strcmp(r->label, "flgr")) out = cJSON_CreateNumber((double) kn_json_int(v, "flg", -1));
    else if (!strcmp(r->label, "cbdgr")) out = cJSON_CreateNumber((double) kn_json_int(v, "badge", -1));
    else out = v ? cJSON_Duplicate(v, 1) : cJSON_CreateNull();
    social_send_n(s, r->label, 1, out);
    cJSON_Delete(v);
}

void sh_challenges(SocialSession *s, const cJSON *f) {
    (void) f;
    if (!social_require_auth(s)) return;
    char me[24], now[24];
    social_call(s, &P_CHALLENGES, KN_DB_ARGS(social_viewer(s, me), acct_itoa(now, (int64_t) time(NULL))), prog_done,
                prog_reply("chgL"));
}

void sh_badge_select(SocialSession *s, const cJSON *f) {
    if (!social_require_auth(s)) return;
    char me[24], badge[24];
    social_call(s, &P_BADGE_SELECT, KN_DB_ARGS(social_viewer(s, me), acct_itoa(badge, fa_int(f, 1, -1))), prog_done, prog_reply("bdgr"));
}

/* ["flg", index] -> ["flgr", index] | ["po", null, err] (io_flgr: the account's Country Flag, -1 none).
 * The same label as the staff hacker flag ["flg", name, 0|1]: profiles.c sh_flag tells them apart. */
void sh_country_flag(SocialSession *s, const cJSON *f) {
    if (!social_require_auth(s)) return;
    char me[24], flag[24];
    social_call(s, &P_COUNTRY_FLAG, KN_DB_ARGS(social_viewer(s, me), acct_itoa(flag, fa_int(f, 1, -1))), prog_done, prog_reply("flgr"));
}

void sh_badge_claim(SocialSession *s, const cJSON *f) {
    if (!social_require_auth(s)) return;
    char me[24], badge[24], season[24], now[24];
    social_call(s, &P_BADGE_CLAIM,
                KN_DB_ARGS(social_viewer(s, me), acct_itoa(badge, fa_int(f, 1, -1)), acct_itoa(season, acct_current_season()),
                           acct_itoa(now, (int64_t) time(NULL))),
                prog_done, prog_reply("cbdgr"));
}

void sh_medals(SocialSession *s, const cJSON *f) {
    (void) f;
    if (!social_require_auth(s)) return;
    char me[24];
    social_call(s, &P_MEDALS, KN_DB_ARGS(social_viewer(s, me)), prog_done, prog_reply("mdl"));
}
