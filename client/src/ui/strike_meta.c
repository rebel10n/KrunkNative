// Krunker Strike's account / round meta views that no sandbox capture exists for (strike_ui.h).
// Their markup is TRANSCRIBED from FRVR's Mithril views in assets/index-Cz2uYXJQ.js (read as text;
// names below are its minified identifiers) into ui/strike/screens/<VIEW_ID>.html, main markup
// plus "<!-- part NAME -->" components; nothing here is pixel-verified against FRVR's client.
//
//   UD MISSION_TAB_SCREEN   the Battle Pass screen's (_C) Missions tab: FT tabs [Rewards, Missions],
//                           content `bpData && account.BP ? UD : I2 ("Loading Missions")`; UD = uX
//                           ("Daily missions") + one hX card per account.activeMissions T == "daily",
//                           sorted by PT descending. FRVR's stylesheet has no .frvr-mission-card rules.
//   XB REWARD_TO_CLAIM      wg (the reward card: showType false, middle 145x115, bottom 156x29,
//                           colours reversed) of pa(reward); FRVR shows it in its tournament rewards
//                           popup (qB). Here it sits in the POPUP frame (In) as capture-strike.js
//                           pushes it; KrunkNative has no hosted tournaments, so only a scenario opens it.
//   GB / Ny MATCH_DETAILS_SCREEN  Pt(Ny, {legacyData}) from the round end's podium button: La > gAe
//                           lightbox, "Match Details", the legacy board (FFA: vAe + K5 rows; teams:
//                           yF / bF per team), Close = kt(). TA (Report players) is never reset; a
//                           row's report symbol opens REPORT_PLAYER (Hy, strike_social.c).
//   KU PROGRESSION_TRACK    PROFILE's Rewards Track (Gue columns, level 1..max of the game config's
//                           accountProgression; pa() labels), single track (no premium track) and
//                           its button area. KU renders nothing until the config's item
//                           definitions are in (X2): the live client has no accountProgression
//                           source (FRVR's /v1/game-config), so only a scenario's
//                           state.strike.progression shows it.
//
// KrunkNative's data in FRVR's shapes:
//   missions   account.activeChallenges (kn_brain "chgL", account_challenges()): N or the challenge
//              name, IC, PT (done, the sort key), PV, DAT.val, TR, REW.kr / bpp / jnk. Reward skins
//              (REW.sk, Krunker skins, not FRVR item definitions) are not shown.
//   bpData     battlepass_ui_pass() of a signed-in account (Se.bpData && account.BP)
//   OD REWARDS_TAB_SCREEN  the Battle Pass screen's Rewards tab (the free track; the VIP column and
//              Unlock VIP are not ported: premium money). Claim Rewards claims the first claimable card
//              (claimMassItem: _S(t[0])), each card's click claims it (cX).
//   legacyData the END table (hud_modes_end_rows / _cell: the mode's endStats, the server's endSort
//              order, team totals), else the players by score
#include <strike_ui.h>
#include "klog.h"
#include <html_ui.h>
#include <ui_logic.h>
#include <i18n.h>
#include <backend_ui.h>
#include <time.h>
#include <hud_modes.h>
#include <spectate.h>
#include <modes.h>
#include <season.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strike_econ.h> // a signed-in Strike account's missions and rewards track (kn_brain /strike)

#define MD_ROWS_MAX 64

static struct {
    Client *client;
    int bp_tab;                 // FT viewState.selectedTab of the Battle Pass screen
    unsigned long missions_sig; // what the Missions tab shows (re-filled when the challenges change)
    unsigned long rewards_sig;  // what the Rewards tab shows (the Battle Pass data, BPP and claimed cards)
    int report;                 // TA: Report players was pressed (the board shows report symbols)
    cJSON *progression;         // the game config's accountProgression (owned; NULL = none)
    cJSON *claimed;             // account.progressionClaimedItems (owned)
    cJSON *reward;              // REWARD_TO_CLAIM's reward (owned)
    // kn_brain's Strike missions (GET /strike/v1/missions/me) as UD's cards read them (owned)
    cJSON *econ_missions;       // the answer
    cJSON *econ_cards;          // its daily missions in the challenge shape mission_card() reads
    int econ_missions_asked;    // a request is out / answered for the current state
    int64_t econ_missions_asked_s; // when that request was sent: BATTLEPASS_LOAD_TIMEOUT_S ends it
    int econ_missions_failed;   // the last answer was a refusal (or none): the tab says so instead of "Loading", and a tab tap asks again
    unsigned econ_seq;          // bumped on every answer (the Missions tab re-fills)
    int track_asked;            // the game config (accountProgression) was asked
    int tap_missions;           // TAP_TO_PLAY's showMissions (the Missions logo's toggle)
    unsigned long widget_sig;   // what the daily missions widgets show (re-filled when it changes)
} T;

// A signed-in Strike account (strike_econ.c): missions and the track come from kn_brain's /strike
static int econ_on(void) {
    const StrikeAccount *a = strike_econ_account();
    return a->account_id > 0 && a->loaded;
}

// ---------------------------------------------------------------- helpers

// String(number) for the values FRVR formats with JS: integers plain, else the shortest
// representation that reads back the same.
static void js_num(const double v, char *out, const size_t n) {
    if (v == floor(v) && fabs(v) < 1e15) {
        snprintf(out, n, "%lld", (long long) v);
        return;
    }
    for (int p = 1; p <= 17; p++) {
        snprintf(out, n, "%.*g", p, v);
        if (strtod(out, NULL) == v) return;
    }
}

static double jnum(const cJSON *o, const char *k) {
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
    return cJSON_IsNumber(v) ? v->valuedouble : cJSON_IsBool(v) ? (double) cJSON_IsTrue(v) : 0.0;
}

static const char *jstr(const cJSON *o, const char *k) {
    const char *s = o ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, k)) : NULL;
    return s ? s : "";
}

// N.getReadableTime(ms) (Zj) as hX calls it: no seconds, minutes always ("1h 0m ")
static void readable_time(const double ms, char *out, const size_t n) {
    long long s = (long long) floor(ms / 1000.0), m = s / 60, h = m / 60, d = h / 24;
    const long long y = d / 365;
    m %= 60;
    h %= 24;
    d %= 365;
    size_t len = 0;
    out[0] = 0;
    if (y) len += (size_t) snprintf(out + len, n - len, "%lldy ", y);
    if (d && len < n) len += (size_t) snprintf(out + len, n - len, "%lldd ", d);
    if (h && len < n) len += (size_t) snprintf(out + len, n - len, "%lldh ", h);
    if (len < n) snprintf(out + len, n - len, "%lldm ", m);
}

static void append_part(StrBuf *sb, const char *file, const char *part, const StrikeVar *vars, const int count) {
    char *html = strike_template_part(file, part, vars, count);
    if (html) {
        sb_append(sb, html);
        free(html);
    }
}

static char *part_string(const char *file, const char *part, const StrikeVar *vars, const int count) {
    char *html = strike_template_part(file, part, vars, count);
    return html ? html : calloc(1, 1);
}

// pa(reward): the image and label FRVR shows for a reward; 0 = none (pa returns undefined:
// items / classes need FRVR's item definitions, seasonal tokens an event)
static int reward_info(const cJSON *r, char *image, const size_t in, char *label, const size_t ln) {
    const char *type = jstr(r, "type");
    char amount[32];
    js_num(jnum(r, "amount"), amount, sizeof(amount));
    if (!strcmp(type, "currency")) {
        const char *cur = jstr(r, "currency");
        const double a = jnum(r, "amount");
        if (!strcmp(cur, "kr")) {
            snprintf(image, in, "./img/currencies/krunkies.png");
            snprintf(label, ln, "%sx KR", amount);
        } else if (!strcmp(cur, "spin_ticket")) {
            if (a >= 1 && a <= 3) {
                snprintf(image, in, "./img/frvr-ui/spin-ticket-%s.png", amount);
                snprintf(label, ln, "%sx Spin Ticket", amount);
            } else {
                snprintf(image, in, "./img/frvr-ui/spin-ticket-many.png");
                snprintf(label, ln, "%sx Spin Tickets", amount);
            }
        } else if (!strcmp(cur, "xp")) {
            snprintf(image, in, "./img/frvr-ui/xp-symbol.svg");
            snprintf(label, ln, "%sx XP", amount);
        } else {
            return 0;
        }
        return 1;
    }
    if (!strcmp(type, "battlepass")) { // the pass's ticket (FRVR's reward-image--bp art)
        snprintf(image, in, "./img/battlepass/ticket.svg");
        snprintf(label, ln, "Battle Pass");
        return 1;
    }
    if (!strcmp(type, "feature")) {
        static const struct { const char *feature, *image, *label; } FEATURES[] = {
            {"daily_missions", "./img/frvr-ui/missions/missions-icon.svg", "Daily Missions"},
            {"premium_account", "./img/premium/premium-logo.png", "Premium Account"},
            {"no_ads_unsolicited", "./img/features/no-ads-unsolicited-logo.svg", "No Unsolicited Ads"},
            {"xp_boost", "./img/features/xp-boost-logo.svg", "XP Boost"},
        };
        const char *f = jstr(r, "feature");
        for (size_t i = 0; i < sizeof(FEATURES) / sizeof(FEATURES[0]); i++) {
            if (strcmp(f, FEATURES[i].feature) != 0) continue;
            snprintf(image, in, "%s", FEATURES[i].image);
            snprintf(label, ln, "%s", FEATURES[i].label);
            return 1;
        }
    }
    return 0;
}

// ---------------------------------------------------------------- MISSION_TAB_SCREEN (UD / hX)

// chall.name.<key> (tools/strike/strike_data.json i18n, the 1.37.2 English table): {0} the required
// value, {1} the weapon part (none for Strike's daily missions)
static const char *const CHALL_NAMES[][2] = {
    {"airk", "Get {0} mid-air kills {1}"}, {"ast", "Get {0} assists {1}"}, {"bzk", "Get {0} buzz kills {1}"},
    {"cc", "Get {0} close-calls {1}"}, {"d", "Die {0} times"}, {"dmg", "Deal {0} damage {1}"}, {"drk", "Get {0} drift kills {1}"},
    {"fb", "Get {0} first bloods {1}"}, {"fk", "Get {0} fist kills {1}"}, {"gp", "Play {0} matches {1}"},
    {"hs", "Get {0} headshots {1}"}, {"k", "Kill {0} opponents {1}"}, {"kstk", "Get a {0} killstreak {1}"},
    {"lns", "Get {0} longshots {1}"}, {"ls", "Get {0} legshots {1}"}, {"mk", "Get {0} melee kills {1}"},
    {"ns", "Get {0} no-scopes {1}"}, {"qs", "Get {0} quickscopes {1}"}, {"rk", "Deal {0} revenge kills {1}"},
    {"score", "Get {0} score"}, {"tk", "Get {0} turbo kills {1}"}, {"tmk", "Get {0} throwing knife kills {1}"},
    {"w", "Win {0} matches"}, {"wb", "Get {0} wallbangs {1}"},
};

static void chall_name(const char *key, const double value, char *out, const size_t n) {
    const char *tmpl = NULL;
    for (size_t i = 0; key && i < sizeof(CHALL_NAMES) / sizeof(CHALL_NAMES[0]); i++) {
        if (!strcmp(CHALL_NAMES[i][0], key)) tmpl = CHALL_NAMES[i][1];
    }
    if (!tmpl) {
        snprintf(out, n, "chall.name.%s", key ? key : ""); // i18n.t of a missing key prints the key
        return;
    }
    char val[32];
    js_num(value, val, sizeof(val));
    size_t o = 0;
    for (const char *p = tmpl; *p && o + 1 < n; p++) {
        if (p[0] == '{' && (p[1] == '0' || p[1] == '1') && p[2] == '}') {
            for (const char *v = p[1] == '0' ? val : ""; *v && o + 1 < n; v++) out[o++] = *v;
            p += 2;
        } else {
            out[o++] = *p;
        }
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    out[o] = 0;
}

// /missions/me's dailyMissions.missions [{details: {_id, key, requiredValue, rewards}, progress, completed,
// claimed}] as the challenge cards hX reads: N, IC (the mission's catalogue place: FRVR's icon index is
// server data), PV, PT, TR (the day's refresh), DAT.val, REW.kr
static void econ_cards_build(void) {
    cJSON_Delete(T.econ_cards);
    T.econ_cards = cJSON_CreateArray();
    const cJSON *daily = cJSON_GetObjectItemCaseSensitive(T.econ_missions, "dailyMissions");
    const double tr = jnum(daily, "timeSecondsToRefresh") * 1000.0;
    const cJSON *m;
    cJSON_ArrayForEach(m, cJSON_GetObjectItemCaseSensitive(daily, "missions")) {
        const cJSON *det = cJSON_GetObjectItemCaseSensitive(m, "details");
        const char *id = jstr(det, "_id"), *key = jstr(det, "key");
        cJSON *c = cJSON_CreateObject();
        char name[160];
        chall_name(key, jnum(det, "requiredValue"), name, sizeof(name));
        cJSON_AddStringToObject(c, "T", "daily");
        cJSON_AddStringToObject(c, "N", name);
        int ic = 0;
        const StrikeEconomy *e = strike_economy();
        for (int i = 0; i < e->mission_count; i++) {
            if (!strcmp(e->missions[i].id, id)) ic = i;
        }
        cJSON_AddNumberToObject(c, "IC", ic);
        cJSON_AddNumberToObject(c, "PV", jnum(m, "progress"));
        cJSON_AddNumberToObject(c, "PT", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "completed")) ? 1 : 0);
        cJSON_AddNumberToObject(c, "TR", tr);
        cJSON_AddNumberToObject(cJSON_AddObjectToObject(c, "DAT"), "val", jnum(det, "requiredValue"));
        double kr = 0.0;
        const cJSON *r;
        cJSON_ArrayForEach(r, cJSON_GetObjectItemCaseSensitive(det, "rewards")) {
            if (!strcmp(jstr(r, "type"), "currency") && !strcmp(jstr(r, "currency"), "kr")) kr += jnum(r, "amount");
        }
        cJSON_AddNumberToObject(cJSON_AddObjectToObject(c, "REW"), "kr", kr);
        cJSON_AddStringToObject(c, "_id", id);
        cJSON_AddBoolToObject(c, "claimable", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "completed"))
                                                  && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "claimed")));
        cJSON_AddItemToArray(T.econ_cards, c);
    }
}

static void on_missions(void *user, const int status, const cJSON *data, const cJSON *error) {
    (void) user;
    (void) error;
    if (status != 200 || !cJSON_IsObject(data)) {
        printf("[strike] missions refused (%d)\n", status);
        T.econ_missions_failed = 1; // the answer is in: the tab says so (a tap on it asks again)
        return;
    }
    T.econ_missions_failed = 0;
    cJSON_Delete(T.econ_missions);
    T.econ_missions = cJSON_Duplicate(data, 1);
    econ_cards_build();
    T.econ_seq++;
    KN_LOG_VERBOSE2("[strike] missions: %d daily%s\n", cJSON_GetArraySize(T.econ_cards),
           cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "locked")) ? " (locked)" : "");
}

static void econ_missions_ask(void) {
    if (T.econ_missions_asked || !econ_on()) return;
    T.econ_missions_asked = strike_econ_get_missions(on_missions, NULL);
    T.econ_missions_asked_s = (int64_t) time(NULL);
    // sent: the tab is loading again (a Retry's re-ask clears the failed text); not sent (no brain socket): the tab says so now, with Retry
    T.econ_missions_failed = !T.econ_missions_asked;
}

// Se.bpData && account.BP; a Strike account: its missions answer, unlocked
static int bp_has_data(void) {
    if (econ_on()) return T.econ_missions && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(T.econ_missions, "locked"));
    return active_account() && battlepass_ui_pass();
}

static const cJSON *missions_list(void) { return econ_on() ? T.econ_cards : account_challenges(); }

static int daily_missions(const cJSON **out, const int max) {
    const cJSON *list = missions_list(), *c;
    int n = 0;
    cJSON_ArrayForEach(c, list) {
        if (n < max && !strcmp(jstr(c, "T"), "daily")) out[n++] = c;
    }
    // e.sort((t, n) => n.PT - t.PT): stable
    for (int i = 1; i < n; i++) {
        const cJSON *x = out[i];
        int j = i - 1;
        while (j >= 0 && jnum(out[j], "PT") < jnum(x, "PT")) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = x;
    }
    return n;
}

static unsigned long missions_signature(void) {
    const cJSON *list = missions_list(), *c;
    unsigned long sig = (unsigned long) (uintptr_t) list * 31u + (unsigned long) bp_has_data() + T.econ_seq * 977u
                        + (unsigned long) T.econ_missions_failed * 7919u;
    cJSON_ArrayForEach(c, list) sig = sig * 131u + (unsigned long) (jnum(c, "PV") * 7 + jnum(c, "PT") * 3 + jnum(c, "TR") / 60000.0);
    return sig;
}

static char *mission_card(const cJSON *c) {
    const cJSON *dat = cJSON_GetObjectItemCaseSensitive(c, "DAT"), *rew = cJSON_GetObjectItemCaseSensitive(c, "REW");
    char title[192], icon[64], progress[48], text[64], buf[64];
    const char *n = jstr(c, "N");
    if (n[0]) snprintf(title, sizeof(title), "%s", n);
    else account_challenge_name(c, title, sizeof(title));
    // N.assetsUrl("img/chals/icon_" + (IC || 0) + ".png"): cdn.frvr.com, vendored under ./cdn/
    snprintf(icon, sizeof(icon), "./cdn/img/chals/icon_%d.png", (int) jnum(c, "IC"));
    const int complete = jnum(c, "PT") != 0.0;
    const double cur = jnum(c, "PV"), target = jnum(dat, "val");
    // hX reads n.key, which UD puts under n.data: the "time" branch never runs in 1.37.2
    char a[24], b[24];
    js_num(cur, a, sizeof(a));
    js_num(target, b, sizeof(b));
    if (complete) snprintf(text, sizeof(text), "Completed");
    else snprintf(text, sizeof(text), "%s/%s", a, b);
    // (complete ? 100 : current / target * 100) + "%": JS prints 0 / 0 as NaN
    const double pct = complete ? 100.0 : target != 0.0 ? cur / target * 100.0 : NAN;
    if (isnan(pct)) snprintf(buf, sizeof(buf), "NaN");
    else js_num(pct, buf, sizeof(buf));
    snprintf(progress, sizeof(progress), "%s%%", buf);

    char *timeleft = NULL;
    if (jnum(c, "TR") != 0.0) {
        char t[48];
        readable_time(jnum(c, "TR"), t, sizeof(t));
        const StrikeVar tv[] = {{"time", t}};
        timeleft = part_string("MISSION_TAB_SCREEN", "timeleft", tv, 1);
    }
    StrBuf rs = {0};
    const double bpp = jnum(rew, "bpp"), kr = jnum(rew, "kr"), junk = jnum(rew, "jnk");
    if (bpp) {
        js_num(bpp, buf, sizeof(buf));
        const StrikeVar v[] = {{"value", buf}};
        append_part(&rs, "MISSION_TAB_SCREEN", "bpp", v, 1);
    }
    if (kr) {
        js_num(kr, buf, sizeof(buf));
        const StrikeVar v[] = {{"value", buf}};
        append_part(&rs, "MISSION_TAB_SCREEN", "kr", v, 1);
    }
    if (junk) {
        js_num(junk, buf, sizeof(buf));
        const StrikeVar v[] = {{"value", buf}};
        append_part(&rs, "MISSION_TAB_SCREEN", "junk", v, 1);
    }
    char *rewards = sb_finish(&rs);
    const StrikeVar vars[] = {{"icon", icon}, {"title", title}, {"timeleft", timeleft ? timeleft : ""},
                              {"rewards", rewards ? rewards : ""}, {"progress", progress}, {"progressText", text}};
    char *html = part_string("MISSION_TAB_SCREEN", "card", vars, 6);
    free(timeleft);
    free(rewards);
    return html;
}

// ---------------------------------------------------------------- REWARDS_TAB_SCREEN (OD / cX)

// OD's populateRewardList for the free track: a card is claimable when its level is reached and it
// is not claimed (level col <= the account's level). claimMassItem claims the first one.
typedef struct {
    int level;     // the account's Battle Pass level (bp_level)
    int claimable; // cards that can be claimed now
    int first;     // the first claimable card's id (0 none)
} BpScan;

static BpScan bp_scan(const BattlePass *bp, const int64_t bpp) {
    BpScan s = {0, 0, 0};
    if (!bp) return s;
    s.level = bp_level(bp, bpp, NULL, NULL);
    const char *claimed = battlepass_ui_claimed();
    for (int col = 1; col <= bp->item_count && col <= s.level; col++) {
        const BattlePassItem *it = bp_item_at(bp, 0, col - 1);
        if (!it || bp_is_claimed(claimed, it->id)) continue;
        if (!s.claimable++) s.first = it->id;
    }
    return s;
}

// cX's card slot: the skin's thumbnail, else KR / junk (sk > kr > jn), its text (kr " KR", jn)
static char *bp_slot(const BattlePassItem *it, const int claimed) {
    char image[256] = "", label[64] = "", num[32];
    int junk = 0;
    if (it->skin > 0) store_skin_preview(it->skin, image, sizeof image);
    else if (it->kr > 0) snprintf(image, sizeof image, "./img/currencies/krunkies.png");
    else if (it->jnk > 0) junk = 1;
    if (it->kr > 0) snprintf(label, sizeof label, "%d KR", it->kr);
    else if (it->jnk > 0) {
        js_num(it->jnk, num, sizeof num);
        snprintf(label, sizeof label, "%s", num);
    }
    char *check = claimed ? part_string("REWARDS_TAB_SCREEN", "check", NULL, 0) : NULL;
    const StrikeVar iv[] = {{"src", image}};
    char *img = junk ? part_string("REWARDS_TAB_SCREEN", "junk", NULL, 0)
              : image[0] ? part_string("REWARDS_TAB_SCREEN", "image", iv, 1) : NULL;
    const StrikeVar tv[] = {{"label", label}};
    char *text = label[0] ? part_string("REWARDS_TAB_SCREEN", "text", tv, 1) : NULL;
    const StrikeVar v[] = {{"check", check ? check : ""}, {"image", img ? img : ""}, {"text", text ? text : ""}};
    char *slot = part_string("REWARDS_TAB_SCREEN", "slot", v, 3);
    free(check);
    free(img);
    free(text);
    return slot;
}

// one column of the treadmill (OD's cX on the free track): the level, the card, its claim state
static char *bp_column(const BattlePass *bp, const int col, const int level, const char *claimed) {
    const BattlePassItem *it = bp_item_at(bp, 0, col - 1);
    const int done = it && bp_is_claimed(claimed, it->id);
    const int reached = col <= level;
    char lv[16], column_class[64] = "", number_class[80] = "", card_class[96] = "", onclick[64] = "";
    snprintf(lv, sizeof lv, "%d", col);
    if (col > level) snprintf(column_class, sizeof column_class, " frvr-bp-rewards-tab__column--locked");
    if (col == level) snprintf(number_class, sizeof number_class, " frvr-bp-rewards-tab__col-number--current");
    else if (col < level) snprintf(number_class, sizeof number_class, " frvr-bp-rewards-tab__col-number--previous");
    if (done || reached) snprintf(card_class, sizeof card_class, " frvr-bp-rewards-tab__card--claimable-claimed");
    if (it && !done && reached) snprintf(onclick, sizeof onclick, " onclick=\"strikeAction('bp-claim:%d')\"", it->id);
    char *slot = it ? bp_slot(it, done) : NULL;
    const StrikeVar v[] = {{"columnClass", column_class}, {"numberClass", number_class}, {"level", lv}, {"cardClass", card_class},
                           {"onclick", onclick}, {"slot", slot ? slot : ""}};
    char *html = part_string("REWARDS_TAB_SCREEN", "column", v, 6);
    free(slot);
    return html;
}

// OD's lX header: the season's name and its end, the level, the BPP into it, the Available BP bar
// (bp_cap_total / bp_available: the daily cap)
static char *bp_header(const BattlePass *bp, const int64_t bpp) {
    const int64_t now = battlepass_ui_now();
    int64_t points = 0;
    int complete = 0;
    const int level = bp_level(bp, bpp, &points, &complete);
    const int64_t need = bp_level_need(bp, level);
    const int64_t cap = bp_cap_total(bp, now), avail = bp_available(bp, bpp, now);
    char end[64], season[256], lvl[32], xp[64], xp_pct[32], avail_pct[32];
    readable_time(bp->end_s > now ? (double) (bp->end_s - now) * 1000.0 : 0.0, end, sizeof end);
    snprintf(season, sizeof season, "%s", bp->name);
    snprintf(lvl, sizeof lvl, "Level %d", level);
    snprintf(xp, sizeof xp, "BPP: %lld / %lld", (long long) points, (long long) need);
    js_num(need > 0 ? (double) points / (double) need * 100.0 : 0.0, xp_pct, sizeof xp_pct);
    js_num(cap > 0 ? (double) avail / (double) cap * 100.0 : 0.0, avail_pct, sizeof avail_pct);
    const StrikeVar v[] = {{"season", season}, {"endText", end}, {"levelText", lvl}, {"xpText", xp}, {"xpPct", xp_pct}, {"availPct", avail_pct}};
    return part_string("REWARDS_TAB_SCREEN", "header", v, 6);
}

// OD's view: the header, the free track's treadmill (every level's column), Claim Rewards
static char *rewards_html(void) {
    const BattlePass *bp = battlepass_ui_pass();
    const int64_t bpp = battlepass_ui_bpp();
    const char *claimed = battlepass_ui_claimed();
    const BpScan scan = bp_scan(bp, bpp);
    StrBuf cols = {0};
    for (int col = 1; bp && col <= bp->item_count; col++) {
        char *c = bp_column(bp, col, scan.level, claimed);
        sb_append(&cols, c);
        free(c);
    }
    char *columns = sb_finish(&cols);
    char *header = bp_header(bp, bpp);
    char *claim = part_string("REWARDS_TAB_SCREEN", scan.claimable > 0 ? "claim" : "claim-disabled", NULL, 0);
    const StrikeVar v[] = {{"header", header ? header : ""}, {"columns", columns ? columns : ""}, {"claim", claim}};
    char *html = strike_template("REWARDS_TAB_SCREEN", v, 3);
    KN_LOG_VERBOSE2("[strike] REWARDS_TAB_SCREEN %d levels, level %d, %d claimable\n", bp ? bp->item_count : 0, scan.level, scan.claimable);
    free(header);
    free(columns);
    free(claim);
    return html;
}

static unsigned long rewards_signature(void) {
    const BattlePass *bp = battlepass_ui_pass();
    // the load's status too: a failed "bpdl" (battlepass_ui_tick, 15 s) changes no data, and the tab must say so
    unsigned long sig = (unsigned long) (uintptr_t) bp * 31u + (unsigned long) bp_has_data() + (unsigned long) battlepass_ui_bpp() * 7u
                        + (unsigned long) battlepass_ui_status() * 1009u;
    for (const char *p = battlepass_ui_claimed(); p && *p; p++) sig = sig * 131u + (unsigned char) *p;
    return sig;
}

// the free track's claim of one card (windows[5].claimItem): the answer is a popup (strike_profile.c)
static void bp_claim(const int id) {
    strike_popup_expect(NULL);
    battlepass_ui_claim(id);
}

// FT: the tab selector's .selected and the tab's content (g(UD) or g(I2, {screen}))
static void bp_tab(const HtmlElem root, const int tab) {
    T.bp_tab = tab;
    HtmlElem items[4];
    const int n = html_dom_query_all(root, ".tabSelectorItem", items, 4);
    for (int i = 0; i < n; i++) html_dom_set_class(items[i], "selected", i == tab);
    const HtmlElem content = html_dom_query(root, ".tabsContent");
    if (!content) return;
    char *html = NULL;
    if ((tab == 1 || T.econ_missions_failed) && !T.econ_missions_asked) econ_missions_ask(); // a Strike account: GET /strike/v1/missions/me
    if (tab == 0 && bp_has_data()) {
        html = rewards_html();
    } else if (tab == 1 && bp_has_data()) {
        if (!econ_on()) account_challenges_request(); // account.activeMissions: asked once per session
        const cJSON *list[64];
        const int count = daily_missions(list, 64);
        StrBuf cards = {0};
        for (int i = 0; i < count; i++) {
            char *card = mission_card(list[i]);
            sb_append(&cards, card);
            free(card);
        }
        char *body = sb_finish(&cards);
        const StrikeVar v[] = {{"missions", body ? body : ""}};
        html = strike_template("MISSION_TAB_SCREEN", v, 1);
        free(body);
        KN_LOG_VERBOSE2("[strike] MISSION_TAB_SCREEN %d daily missions\n", count);
    } else if (tab == 0 && battlepass_ui_status() == 3) {
        // the "bpdl" was not sent or had no answer: the tab says so, with a Retry that asks again (bp-tab:0)
        const StrikeVar v[] = {{"message", I18N("kn.strike.rewardsFailed", "Couldn't load the Battle Pass.")}, {"action", "bp-tab:0"},
                               {"retry", I18N("kn.retry", "Retry")}};
        html = strike_template_part("MISSION_TAB_SCREEN", "failed", v, 3);
    } else if (tab == 1 && econ_on() && T.econ_missions_failed) { // refused / no answer in time: the tab says so, with Retry (bp-tab:1 asks again)
        const StrikeVar v[] = {{"message", I18N("kn.strike.missionsFailed", "Couldn't load your missions. Open this tab again to retry.")}, {"action", "bp-tab:1"},
                               {"retry", I18N("kn.retry", "Retry")}};
        html = strike_template_part("MISSION_TAB_SCREEN", "failed", v, 3);
    } else if (tab == 1 && !econ_on() && battlepass_ui_status() == 3) { // a Krunker account's missions come with its pass: the same failure
        const StrikeVar v[] = {{"message", I18N("kn.strike.missionsFailed", "Couldn't load your missions. Open this tab again to retry.")}, {"action", "bp-tab:1"},
                               {"retry", I18N("kn.retry", "Retry")}};
        html = strike_template_part("MISSION_TAB_SCREEN", "failed", v, 3);
    } else if ((tab == 0 || !econ_on()) && battlepass_ui_status() == 2) { // answered with no pass: the window's own text (a Strike account's Missions tab follows its own request)
        html = strdup(I18N("kn.strike.noBattlePass", "No Battle Pass Found"));
    } else if (!econ_on() && !active_account()) {
        // signed out: nothing will load for either tab, so the tab says so (the sign-in copy of the Krunker
        // market, the FRVR page has none for this state); never "Loading" for good
        const StrikeVar v[] = {{"message", I18N("social.market.login", "You must be logged in.")}};
        html = strike_template_part("MISSION_TAB_SCREEN", "signedout", v, 1);
    } else {
        // I2: "Loading Rewards" / "Loading Missions" (no bpData, signed in and the answer is on its way)
        const StrikeVar v[] = {{"screen", tab == 1 ? "Missions" : "Rewards"}};
        html = strike_template_part("MISSION_TAB_SCREEN", "loading", v, 1);
    }
    if (html) {
        html_dom_set_html(content, html);
        free(html);
    }
    // KrunkNative: a completed Strike mission pays out when its card is clicked (POST
    // /strike/v1/missions/claim/:id); FRVR 1.37.2's hX card has no claim of its own
    if (tab == 1 && econ_on() && bp_has_data()) {
        const cJSON *list[64];
        const int count = daily_missions(list, 64);
        HtmlElem cards[64];
        const int cn = html_dom_query_all(content, ".frvr-mission-card", cards, 64);
        for (int i = 0; i < count && i < cn; i++) {
            if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(list[i], "claimable"))) continue;
            char hook[96];
            snprintf(hook, sizeof(hook), "strikeAction('mission-claim:%s')", jstr(list[i], "_id"));
            html_dom_set_attr(cards[i], "onclick", hook);
        }
    }
    T.missions_sig = missions_signature();
    T.rewards_sig = rewards_signature();
}

static void bp_render(const HtmlElem root) {
    HtmlElem items[4];
    const int n = html_dom_query_all(root, ".tabSelectorItem", items, 4);
    char hook[48];
    for (int i = 0; i < n; i++) {
        snprintf(hook, sizeof(hook), "strikeAction('bp-tab:%d')", i);
        html_dom_set_attr(items[i], "onclick", hook);
    }
    battlepass_ui_request(); // "bpdl" once (io_accountResponse): the Rewards tab fills when it arrives
    bp_tab(root, 0);         // a new FT: viewState.selectedTab 0
}

// ---------------------------------------------------------------- REWARD_TO_CLAIM (XB / wg)

static void reward_render(const HtmlElem root) {
    char image[96] = "", label[96] = "";
    char *card = NULL;
    if (T.reward && reward_info(T.reward, image, sizeof(image), label, sizeof(label))) {
        const StrikeVar v[] = {{"image", image}, {"label", label}};
        card = strike_template_part("REWARD_TO_CLAIM", "card", v, 2);
    }
    const HtmlElem holder = html_dom_query(root, ".reward-to-claim");
    if (holder) html_dom_set_html(holder, card ? card : ""); // wg: nothing until pa() resolves
    free(card);
}

// ---------------------------------------------------------------- MATCH_DETAILS_SCREEN (GB)

typedef struct {
    const Player *p;
    int rank;
} MdRow;

static int md_rows(const Client *client, MdRow *rows, const int max) {
    int uids[MD_ROWS_MAX];
    int n = 0;
    const int count = hud_modes_end_rows(uids, MD_ROWS_MAX);
    for (int i = 0; i < count && n < max; i++) {
        const Player *p = client_find_player((Client *) client, uids[i]);
        if (p) rows[n++] = (MdRow) {p, i + 1}; // y = p / endStats.length + 1: the place in endData.ed
    }
    if (n) return n;
    // no END table (an offline round without one): the players in the board's order, score first
    for (size_t i = 0; i < client->game.player_count && n < max; i++) {
        const Player *p = client->game.players[i];
        if (p) rows[n++] = (MdRow) {p, 0};
    }
    for (int i = 1; i < n; i++) {
        const MdRow x = rows[i];
        int j = i - 1;
        while (j >= 0 && rows[j].p->score < x.p->score) {
            rows[j + 1] = rows[j];
            j--;
        }
        rows[j + 1] = x;
    }
    for (int i = 0; i < n; i++) rows[i].rank = i + 1;
    return n;
}

// the stat columns: endStats.slice(2) without "reward" (the map skips it)
static int md_stats(const Client *client, const char **out, const int max) {
    const char *const *stats = hud_modes_end_stats(client);
    int n = 0;
    for (int i = 2; stats && stats[i] && n < max; i++) {
        if (strcmp(stats[i], "reward") != 0) out[n++] = stats[i];
    }
    return n;
}

// the header cells (vAe / yF): the stat names as endStats spells them, odd ones --odd
static char *md_header_cols(const char **stats, const int n) {
    StrBuf sb = {0};
    for (int i = 0; i < n; i++) {
        const StrikeVar v[] = {{"class", i % 2 ? "frvr-lboard-end-screen__stat-col frvr-lboard-end-screen__stat-col--odd"
                                               : "frvr-lboard-end-screen__stat-col"},
                               {"value", stats[i]}};
        append_part(&sb, "MATCH_DETAILS_SCREEN", "title-col", v, 2);
    }
    return sb_finish(&sb);
}

static char *md_row(const MdRow *r, const Player *me, const char **stats, const int n, const int index, const int team, const int ffa) {
    const int mine = r->p == me;
    StrBuf cols = {0};
    for (int i = 0; i < n; i++) {
        char value[96] = "";
        hud_modes_end_cell(r->p, stats[i], value, sizeof(value));
        const StrikeVar v[] = {{"class", mine ? "frvr-lboard-end-screen__stat-col frvr-lboard-end-screen__stat-col--player"
                                         : i % 2 ? "frvr-lboard-end-screen__stat-col frvr-lboard-end-screen__stat-col--odd"
                                                 : "frvr-lboard-end-screen__stat-col"},
                               {"value", value}};
        append_part(&cols, "MATCH_DETAILS_SCREEN", "stat-col", v, 2);
    }
    char *col_html = sb_finish(&cols);
    char *report = NULL;
    if (T.report && !mine) {
        char sid[16];
        snprintf(sid, sizeof(sid), "%d", r->p->uid);
        const StrikeVar v[] = {{"sid", sid}};
        report = part_string("MATCH_DETAILS_SCREEN", "report-symbol", v, 1);
    }
    char idx[16], rank[16], pfp[64];
    snprintf(idx, sizeof(idx), "%d", index);
    snprintf(rank, sizeof(rank), "%d", r->rank);
    // N.handleProfilePicture(pfp, undefined, classIndex): no pfp -> the class icon on the CDN
    snprintf(pfp, sizeof(pfp), "./cdn/textures/classes/icon_%d.png", r->p->class_index);
    // K5 (FFA): --player / --ffa; bF (teams): --player / (team ? none : --enemy-team)
    const char *position = mine ? "frvr-lboard-end-screen__position frvr-lboard-end-screen__position--player"
                         : ffa  ? "frvr-lboard-end-screen__position frvr-lboard-end-screen__position--ffa"
                         : team ? "frvr-lboard-end-screen__position"
                                : "frvr-lboard-end-screen__position frvr-lboard-end-screen__position--enemy-team";
    const StrikeVar v[] = {
        {"index", idx}, {"positionClass", position}, {"rank", rank}, {"pfp", pfp},
        {"nameClass", mine ? "frvr-lboard-end-screen__player-name frvr-lboard-end-screen__player-name--player" : "frvr-lboard-end-screen__player-name"},
        {"name", r->p->name}, {"report", report ? report : ""}, {"stats", col_html ? col_html : ""},
    };
    char *html = part_string("MATCH_DETAILS_SCREEN", "row", v, 8);
    free(col_html);
    free(report);
    return html;
}

static char *md_team_header(const int mine, const int score, const char *cols, const int index) {
    char idx[16], s[16];
    snprintf(idx, sizeof(idx), "%d", index);
    snprintf(s, sizeof(s), "%d", score);
    char *report = mine && !T.report ? part_string("MATCH_DETAILS_SCREEN", "report", NULL, 0) : NULL;
    const StrikeVar v[] = {
        {"index", idx},
        {"rowClass", mine ? "frvr-lboard-end-screen__row--player-team" : "frvr-lboard-end-screen__row--enemy-team"},
        {"positionClass", mine ? "frvr-lboard-end-screen__position" : "frvr-lboard-end-screen__position frvr-lboard-end-screen__position--enemy-team"},
        {"label", mine ? "Your team: " : "Enemy team: "},
        {"scoreClass", mine ? "frvr-lboard-end-screen__player-name--blue" : "frvr-lboard-end-screen__player-name--red"},
        {"score", s}, {"report", report ? report : ""}, {"stats", cols},
    };
    char *html = part_string("MATCH_DETAILS_SCREEN", "team-header", v, 8);
    free(report);
    return html;
}

static void md_render(const HtmlElem root) {
    Client *client = T.client;
    const HtmlElem board = html_dom_query(root, ".match-details__legacy-board");
    if (!client || !board) return;
    const char *stats[16];
    const int sn = md_stats(client, stats, 16);
    MdRow rows[MD_ROWS_MAX];
    const int rn = md_rows(client, rows, MD_ROWS_MAX);
    const Player *me = client->me;
    char *cols = md_header_cols(stats, sn);
    char *html = NULL;
    const int ffa = !mode_is_team(&client->game);
    if (ffa) {
        StrBuf sb = {0};
        char *report = !T.report ? part_string("MATCH_DETAILS_SCREEN", "report", NULL, 0) : NULL;
        const StrikeVar hv[] = {{"report", report ? report : ""}, {"stats", cols ? cols : ""}};
        append_part(&sb, "MATCH_DETAILS_SCREEN", "ffa-header", hv, 2);
        free(report);
        for (int i = 0; i < rn; i++) {
            char *row = md_row(&rows[i], me, stats, sn, i + 1, 0, 1);
            sb_append(&sb, row);
            free(row);
        }
        char *body = sb_finish(&sb);
        const StrikeVar v[] = {{"rows", body ? body : ""}};
        html = strike_template_part("MATCH_DETAILS_SCREEN", "ffa", v, 1);
        free(body);
    } else {
        const int my_team = me ? me->team : 0;
        StrBuf mine = {0}, enemy = {0};
        // indices: yF 0, my team 1..t, the enemy's yF t + 1, its rows t + 2.. (o / r of computeMatchDetailsData)
        int t = 0;
        for (int i = 0; i < rn; i++) {
            if (rows[i].p->team != my_team) continue;
            char *row = md_row(&rows[i], me, stats, sn, ++t, 1, 0);
            sb_append(&mine, row);
            free(row);
        }
        int u = 0;
        for (int i = 0; i < rn; i++) {
            if (rows[i].p->team == my_team) continue;
            char *row = md_row(&rows[i], me, stats, sn, u++ + t + 2, 0, 0);
            sb_append(&enemy, row);
            free(row);
        }
        const int other = my_team == 1 ? 2 : 1;
        char *mh = md_team_header(1, hud_modes_end_team_score(my_team ? my_team : 1), cols ? cols : "", 0);
        char *eh = md_team_header(0, hud_modes_end_team_score(my_team ? other : 2), cols ? cols : "", t + 1);
        char *mr = sb_finish(&mine), *er = sb_finish(&enemy);
        const StrikeVar v[] = {{"mine", mh ? mh : ""}, {"mineRows", mr ? mr : ""}, {"enemy", eh ? eh : ""}, {"enemyRows", er ? er : ""}};
        html = strike_template_part("MATCH_DETAILS_SCREEN", "teams", v, 4);
        free(mh);
        free(eh);
        free(mr);
        free(er);
    }
    html_dom_set_html(board, html ? html : "");
    KN_LOG_VERBOSE2("[strike] MATCH_DETAILS_SCREEN %s, %d players, %d stat columns\n", ffa ? "ffa" : "teams", rn, sn);
    free(html);
    free(cols);
}

// ---------------------------------------------------------------- PROGRESSION_TRACK (KU / Gue)

static int track_max_level(void) {
    int max = 0;
    const cJSON *slot;
    cJSON_ArrayForEach(slot, T.progression) {
        const int lv = slot->string ? atoi(slot->string) : 0;
        if (lv > max) max = lv;
    }
    return max;
}

static int track_claimed(const char *slot_id) {
    if (econ_on() && strike_econ_track_claimed(slot_id)) return 1; // progressionClaimedItems
    const cJSON *c;
    cJSON_ArrayForEach(c, T.claimed) {
        if (cJSON_IsString(c) && !strcmp(c->valuestring, slot_id)) return 1;
    }
    return 0;
}

// one Gue column (the free track only: showBothTracks is false)
static char *track_column(const int level, const int player_level, int *claimable) {
    char key[16];
    snprintf(key, sizeof(key), "%d", level);
    const cJSON *slot = cJSON_GetObjectItemCaseSensitive(T.progression, key);
    const cJSON *items = slot ? cJSON_GetObjectItemCaseSensitive(slot, "items") : NULL;
    const int item_count = cJSON_IsArray(items) ? cJSON_GetArraySize(items) : 0;
    const int has_items = cJSON_IsArray(items);
    const int claimed = slot && track_claimed(jstr(slot, "slotId"));
    if (slot && !claimed && level <= player_level) (*claimable)++; // WU(d, level, false)
    char width[16], lv[16];
    snprintf(width, sizeof(width), "%d", item_count ? 45 * item_count + 7 : 45 + 7);
    snprintf(lv, sizeof(lv), "%d", level);
    const char *number = level == player_level ? "frvr-progression-track-rewards__col-number--current"
                       : level < player_level  ? "frvr-progression-track-rewards__col-number--previous"
                                               : "frvr-progression-track-rewards__column--locked";
    char card[192];
    snprintf(card, sizeof(card), "frvr-progression-track-rewards__card%s",
             claimed ? " frvr-progression-track-rewards__card--claimable-claimed"
             : has_items && level <= player_level ? " frvr-progression-track-rewards__card--claimable-idle"
             : has_items ? " frvr-progression-track-rewards__column--locked" : "");
    char label[128] = "";
    char *img_slot = NULL;
    if (slot) {
        StrBuf images = {0};
        const cJSON *it;
        int rewards = 0;
        cJSON_ArrayForEach(it, items) {
            char image[96], text[96];
            if (!reward_info(it, image, sizeof(image), text, sizeof(text))) continue;
            if (!rewards++) snprintf(label, sizeof(label), "%s", text);
            const StrikeVar v[] = {{"src", image}};
            append_part(&images, "PROGRESSION_TRACK", "image", v, 1);
        }
        char *imgs = sb_finish(&images);
        char *check = claimed ? part_string("PROGRESSION_TRACK", "check", NULL, 0) : NULL;
        char *pick = !claimed && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(slot, "isPlayerChoice"))
                         ? part_string("PROGRESSION_TRACK", "pick-one", NULL, 0) : NULL;
        const StrikeVar v[] = {{"check", check ? check : ""}, {"pick", pick ? pick : ""}, {"images", imgs ? imgs : ""}};
        img_slot = part_string("PROGRESSION_TRACK", "img-slot", v, 3);
        free(imgs);
        free(check);
        free(pick);
        const char *bundle = jstr(slot, "bundleName");
        if (claimed) snprintf(label, sizeof(label), "Claimed");
        else if (bundle[0]) snprintf(label, sizeof(label), "%s", bundle);
    }
    const StrikeVar v[] = {{"width", width}, {"numberClass", number}, {"level", lv}, {"cardClass", card},
                           {"slot", img_slot ? img_slot : ""}, {"label", label}};
    char *html = part_string("PROGRESSION_TRACK", "column", v, 6);
    free(img_slot);
    return html;
}

// GET /strike/v1/game-config: its accountProgression is the track ({"<level>": {slotId, items,
// isPlayerChoice}}, the same shape a scenario's state.strike.progression has)
static void on_game_config(void *user, const int status, const cJSON *data, const cJSON *error) {
    (void) user;
    (void) error;
    const cJSON *prog = status == 200 ? cJSON_GetObjectItemCaseSensitive(data, "accountProgression") : NULL;
    if (!cJSON_IsObject(prog)) {
        T.track_asked = 0;
        return;
    }
    cJSON_Delete(T.progression);
    T.progression = cJSON_Duplicate(prog, 1);
    strike_screen_rerender(STRIKE_SCREEN_PROFILE);
}

static void track_ask(void) { T.track_asked = strike_econ_get_game_config(on_game_config, NULL); }

static void on_claimed(void *user, const int status, const cJSON *data, const cJSON *error) {
    (void) data;
    const char *msg = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(error, "message"));
    if (status == 200) KN_LOG_DEBUG1("[strike] claimed %s\n", user ? "a mission" : "a track level");
    else printf("[strike] claim refused (%d): %s\n", status, msg ? msg : "no answer");
    if (user) { // a mission: its list again
        T.econ_missions_asked = 0;
        econ_missions_ask();
    }
}

// gn.claimSlots(l) = Fy: the claimable slots one after another (UU): a pick-one slot (isPlayerChoice) asks
// first (Pt(QC, {options, onChoice})), the claim (claimProgression(slot, choice)) shows its rewards
// (Pt(Es, {rewards, origin: progressionTrack}, {onPop})), and the next slot follows that screen.
static struct {
    char slots[64][24];
    int n, i, busy;
} TQ;

static void track_claim_next(void);

static void track_after_reward(void *user) {
    (void) user;
    track_claim_next(); // Fy: setTimeout(.., 100) then the rest
}

static void on_track_claimed(void *user, const int status, const cJSON *data, const cJSON *error) {
    (void) user;
    const char *msg = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(error, "message"));
    if (status != 200) { // UU: no rewards, no reward screen; Fy stops there
        printf("[strike] claim refused (%d): %s\n", status, msg ? msg : "no answer");
        TQ.busy = 0;
        return;
    }
    KN_LOG_DEBUG1("[strike] claimed a track level\n");
    strike_reward_show(cJSON_GetObjectItemCaseSensitive(data, "rewards"), "progressionTrack", 0, track_after_reward, NULL);
}

static void on_track_choice(void *user, const int choice) {
    (void) user;
    if (TQ.i <= 0 || !strike_econ_claim_level(TQ.slots[TQ.i - 1], choice, on_track_claimed, NULL)) TQ.busy = 0;
}

// the slot's definition: the game config's (accountProgression), else the catalogue's
static const cJSON *track_slot_config(const char *slot_id) {
    const cJSON *s;
    cJSON_ArrayForEach(s, T.progression) {
        const char *id = jstr(s, "slotId");
        if (id && !strcmp(id, slot_id)) return s;
    }
    return NULL;
}

static void track_claim_next(void) {
    if (TQ.i >= TQ.n) {
        TQ.busy = 0;
        KN_LOG_DEBUG1("[strike] Rewards Track: done\n");
        return;
    }
    const char *slot = TQ.slots[TQ.i++];
    const char *ids[8];
    int n = 0, choice = 0;
    const cJSON *cfg = track_slot_config(slot);
    if (cfg) { // UU: i.isPlayerChoice -> the options are its items' itemDefIds
        choice = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cfg, "isPlayerChoice"));
        const cJSON *it;
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(cfg, "items")) {
            const char *id = jstr(it, "itemDefId") ? jstr(it, "itemDefId") : jstr(it, "item");
            if (id && n < 8) ids[n++] = id;
        }
    } else {
        const StrikeTrackSlot *ts = strike_track_slot(slot);
        choice = ts && ts->player_choice;
        for (int k = 0; ts && k < ts->reward_count && n < 8; k++) {
            if (ts->rewards[k].item) ids[n++] = ts->rewards[k].item;
        }
    }
    if (choice && n > 0) {
        strike_reward_choice(ids, n, on_track_choice, NULL);
        return;
    }
    if (!strike_econ_claim_level(slot, 0, on_track_claimed, NULL)) TQ.busy = 0;
}

static void track_render(const HtmlElem profile) {
    const HtmlElem host = html_dom_query(profile, ".frvr-account-progression");
    if (!host) return;
    const HtmlElem old = html_dom_query(host, ".frvr-progression-track-rewards");
    if (old) html_dom_remove(old);
    if (!cJSON_IsObject(T.progression) && econ_on() && !T.track_asked) track_ask(); // the game config
    if (!cJSON_IsObject(T.progression)) return; // !X2: no game config, no track
    const int level = strike_menu_level();
    const int max = track_max_level();
    int claimable = 0;
    StrBuf sb = {0};
    for (int d = 1; d <= max; d++) {
        char *col = track_column(d, level, &claimable);
        sb_append(&sb, col);
        free(col);
    }
    char *columns = sb_finish(&sb);
    char *claim = part_string("PROGRESSION_TRACK", claimable > 0 ? "claim" : "claim-disabled", NULL, 0);
    const StrikeVar v[] = {{"columns", columns ? columns : ""}, {"claim", claim}};
    char *html = strike_template("PROGRESSION_TRACK", v, 2);
    if (html) html_dom_insert_html(host, HTML_INSERT_BEFORE_END, html);
    KN_LOG_VERBOSE2("[strike] PROGRESSION_TRACK %d levels, level %d, %d claimable\n", max, level, claimable);
    free(html);
    free(claim);
    free(columns);
}

// The selectors of one treadmill: KU (PROFILE's Rewards Track) and OD (the Battle Pass Rewards tab)
typedef struct {
    const char *cols, *col, *button, *avail;
} Treadmill;
static const Treadmill TM_TRACK = {".frvr-progression-track-rewards__treadmill-columns", ".frvr-progression-track-rewards__column",
                                   ".frvr-progression-track-rewards__treadmill-button", "frvr-progression-track-rewards__treadmill-button--available"};
static const Treadmill TM_BP = {".frvr-bp-rewards-tab__treadmill-columns", ".frvr-bp-rewards-tab__column",
                                ".frvr-bp-rewards-tab__treadmill-button", "frvr-bp-rewards-tab__treadmill-button--available"};

// KU.slide(n) / OD.slide(n): one page of columns left / right; the arrows' --available follow
static void treadmill_slide(const HtmlElem screen, const Treadmill *tm, const int dir) {
    const HtmlElem cols = screen ? html_dom_query(screen, tm->cols) : 0;
    if (!cols) return;
    HtmlScroll sc = {0};
    if (!html_dom_get_scroll(cols, &sc)) return;
    HtmlElem kids[256];
    const int n = html_dom_query_all(cols, tm->col, kids, 256);
    HtmlRect first = {0};
    if (n > 0) html_dom_get_rect(kids[0], &first);
    const float r = (float) (n - 1) * first.w - sc.client_w;
    const float l = sc.left + (float) dir * sc.client_w, u = sc.client_w + sc.left;
    const int left_ok = !(l <= 0.0f && dir == -1), right_ok = !(u >= r && dir == 1);
    html_dom_set_scroll(cols, l, sc.top);
    HtmlElem buttons[2];
    if (html_dom_query_all(screen, tm->button, buttons, 2) == 2) {
        html_dom_set_class(buttons[0], tm->avail, left_ok);
        html_dom_set_class(buttons[1], tm->avail, right_ok);
    }
}

// ---------------------------------------------------------------- daily missions widget (k5 / hd)

// k5 (Daily Missions): the main menu's box (Xc.Square: grey-box, min width 162) and the Tap-to-Play panel
// (Xc.Simple: the logo, min width 150, its completed missions left out). The bars are hd (the progress bar,
// text on top): the bar is 8 px high, its width * 175 / 193 with 4 / 193 and 10 / 193 at the ends, the
// fill and the completed border by the mission's progress. Shown for an unlocked account only (bp_has_data,
// the Missions tab's gate): a guest or a locked account gets none (FRVR's lock box is not ported).
// The missions are daily_missions() (the Missions tab's list), so completed ones come first: FRVR's k5 keeps
// the server's order, a small deviation kept so the widget and the tab read the same list.
#define MISSION_BAR_DX (175.0 / 193.0)
#define MISSION_BAR_ACE (4.0 / 193.0)
#define MISSION_BAR_OCE (10.0 / 193.0)

// hd's markup for one daily mission at `size` px wide
static void mission_bar(StrBuf *sb, const cJSON *c, const int size, const int simple) {
    const cJSON *dat = cJSON_GetObjectItemCaseSensitive(c, "DAT");
    char title[192], value[64];
    const char *n = jstr(c, "N");
    if (n[0]) snprintf(title, sizeof(title), "%s", n);
    else account_challenge_name(c, title, sizeof(title));
    const int complete = jnum(c, "PT") != 0.0;
    const double target = jnum(dat, "val"), cur = fmin(jnum(c, "PV"), target);
    const double ratio = target != 0.0 ? cur / target : 0.0;
    const char *color = complete ? "progress-bar__green" : "progress-bar__orange";
    char a[24], b[24];
    js_num(cur, a, sizeof(a));
    js_num(target, b, sizeof(b));
    snprintf(value, sizeof(value), complete ? "Completed" : "%s/%s", a, b);
    sb_appendf(sb, "<div class=\"progress-bar__progress-bar-holder progress-bar--text-on-top\" style=\"--progress-width: %d;\">", size);
    sb_appendf(sb, "<div class=\"progress-bar__text-holder\"><div class=\"progress-bar__left-holder progress-bar__font-chakra\" "
                   "style=\"--left-holder-width: %g;\"><span class=\"progress-bar__font-chakra\">", simple ? (double) size : size * 64.8 / 100.0);
    sb_append_esc(sb, title);
    sb_append(sb, " </span><span class=\"progress-bar__font-chakra\"></span></div>");
    if (!simple) {
        sb_append(sb, "<div class=\"progress-bar__right-holder\"><div class=\"progress-bar__value-gained\"></div>"
                      "<div class=\"progress-bar__font-chakra\">");
        sb_append_esc(sb, value);
        sb_append(sb, "</div></div>");
    }
    sb_append(sb, "</div>");
    if (!simple) {
        const double pw = size * MISSION_BAR_DX * ratio;
        char edge[96];
        snprintf(edge, sizeof(edge), ratio == 1.0 ? "progress-bar__bar--border-completed %s" : "progress-bar__bar--border-base", color);
        sb_appendf(sb, "<div class=\"progress-bar__bar-wrapper\" style=\"--bar-height: 8;\">"
                       "<div class=\"progress-bar__bar progress-bar__bar--left %s\" style=\"--pos1: %g; --pos2: %g; --posEnd: 1; --posStart: 0; "
                       "--bar-left-width: %g;\">"
                       "<div class=\"progress-bar__bar-inner %s\" style=\"--bar-progress-width: %g; --bar-height: 6;\"></div>"
                       "<div class=\"progress-bar__bar-inner-inner %s\" style=\"--bar-progress-width: %g; --bar-height: 6;\"></div>"
                       "</div>"
                       "<div class=\"progress-bar__bar progress-bar__bar--middle %s\" style=\"--bar-middle-width: %g;\"></div>"
                       "<div class=\"progress-bar__bar progress-bar__bar--right %s\" style=\"--bar-right-width: %g;\"></div>"
                       "</div>",
                   edge, ratio, ratio, size * MISSION_BAR_DX,
                   color, pw, color, pw,
                   edge, size * MISSION_BAR_ACE,
                   edge, size * MISSION_BAR_OCE);
    }
    sb_append(sb, "</div>");
}

// the box's markup: k5's grey-box (main menu) or the simple panel (Tap-to-Play, with the logo)
static char *missions_box(const int simple) {
    StrBuf bars = {0}, box = {0};
    const cJSON *list[64];
    const int count = daily_missions(list, 64);
    for (int i = 0; i < count; i++) {
        const cJSON *c = list[i];
        if (simple && jnum(c, "PT") != 0.0) continue;
        char *wrap = NULL;
        StrBuf one = {0};
        mission_bar(&one, c, simple ? 150 : 162, simple);
        wrap = sb_finish(&one);
        sb_appendf(&bars, "<div class=\"missions-bar-wrapper \">%s</div>", wrap ? wrap : "");
        free(wrap);
    }
    char *rows = sb_finish(&bars);
    if (simple) {
        sb_appendf(&box, "<div class=\"missions-container missions-container--padding__simple\" style=\"--min-width: 150;\">"
                         "<div class=\"missions-container__simple missions-container__simple--rotate\"></div>"
                         "<img class=\"missions-logo\" src=\"./img/frvr-ui/missions/missions-icon.svg\">"
                         "<div class=\"missions-header\"><span>Daily Missions</span><div class=\"missions-separator\"></div>"
                         "<div class=\"missions-bullet\"></div></div>");
    } else {
        sb_append(&box, "<div class=\"missions-container missions-container--padding__grey-box\" style=\"--min-width: 162;\">"
                        "<div class=\"missions-container__grey-box\"></div>"
                        "<div class=\"missions-header\"><span>Daily Missions</span><div class=\"missions-separator\"></div></div>");
    }
    sb_appendf(&box, "<div class=\"missions-bars-wrapper\">%s</div></div>", rows ? rows : "");
    free(rows);
    return sb_finish(&box);
}

static void widget_fill_main(const HtmlElem root) {
    const HtmlElem host = html_dom_query(root, ".frvr-main-menu--middle-left");
    if (!host) return;
    char *html = bp_has_data() ? missions_box(0) : NULL;
    html_dom_set_html(host, html ? html : "");
    free(html);
}

// the Tap-to-Play panel: .tap-to-play-missions in the top-right, before the info bar, while showMissions
static void widget_fill_tap(const HtmlElem root) {
    const HtmlElem logo = html_dom_query(root, ".tap-to-play-missions-logo");
    if (logo) html_dom_set_attr(logo, "onclick", "strikeAction('tap-missions')");
    const HtmlElem old = html_dom_query(root, ".tap-to-play-missions");
    if (old) html_dom_remove(old);
    const HtmlElem info = html_dom_query(root, ".frvr-main-menu-info-bar");
    if (!info || !T.tap_missions || !bp_has_data()) return;
    char *box = missions_box(1);
    if (!box) return;
    StrBuf wrap = {0};
    sb_appendf(&wrap, "<div class=\"tap-to-play-missions\">%s</div>", box);
    char *html = sb_finish(&wrap);
    html_dom_insert_html(info, HTML_INSERT_BEFORE_BEGIN, html ? html : "");
    free(html);
    free(box);
}

// ---------------------------------------------------------------- hooks

void strike_meta_render(const StrikeScreen s, const HtmlElem root) {
    if (!root) return;
    if (strike_profile_render(s, root)) return; // PROFILE-DETAILS, the message popups (strike_profile.c)
    switch (s) {
    case STRIKE_SCREEN_MAIN_MENU:
        widget_fill_main(root);
        break;
    case STRIKE_SCREEN_TAP_TO_PLAY:
        widget_fill_tap(root);
        break;
    case STRIKE_SCREEN_BATTLEPASS:
        bp_render(root);
        break;
    case STRIKE_SCREEN_REWARD_TO_CLAIM:
        reward_render(root);
        break;
    case STRIKE_SCREEN_MATCH_DETAILS:
        md_render(root);
        break;
    case STRIKE_SCREEN_PROFILE:
        track_render(root);
        break;
    default:
        break;
    }
}

int strike_meta_action(const char *a) {
    if (strike_profile_action(a)) return 1; // Account Details, the Logout and name confirms (strike_profile.c)
    if (!strcmp(a, "tap-missions")) { // showMissions = !showMissions
        T.tap_missions = !T.tap_missions;
        const HtmlElem root = strike_screen_root(STRIKE_SCREEN_TAP_TO_PLAY);
        if (root) widget_fill_tap(root);
        return 1;
    }
    if (!strncmp(a, "bp-tab:", 7)) {
        const HtmlElem root = strike_screen_root(STRIKE_SCREEN_BATTLEPASS);
        const int t = atoi(a + 7);
        if (t == 1 || T.econ_missions_failed) T.econ_missions_asked = 0; // the Missions tab asks for the account's missions on every visit; a failed load is asked again on any tab
        if (t == 0 || (t == 1 && !econ_on())) battlepass_ui_request(); // a "bpdl" that failed (status 3) goes out again; one in flight or answered is not re-sent
        if (root && (t == 0 || t == 1)) bp_tab(root, t);
        return 1;
    }
    if (!strcmp(a, "missions")) { // the Battle Pass screen on its Missions tab
        strike_screen_push(STRIKE_SCREEN_BATTLEPASS);
        const HtmlElem root = strike_screen_root(STRIKE_SCREEN_BATTLEPASS);
        if (root) bp_tab(root, 1);
        return 1;
    }
    if (!strcmp(a, "match-details-report")) { // TA = !0
        T.report = 1;
        strike_screen_rerender(STRIKE_SCREEN_MATCH_DETAILS);
        return 1;
    }
    if (!strncmp(a, "match-details-report-player:", 28)) { // Pt(Hy, {player: t})
        const Player *p = T.client ? client_find_player(T.client, atoi(a + 28)) : NULL;
        if (p) strike_social_report(p->uid, p->name);
        return 1;
    }
    if (!strncmp(a, "track-slide:", 12)) {
        treadmill_slide(strike_screen_root(STRIKE_SCREEN_PROFILE), &TM_TRACK, atoi(a + 12) < 0 ? -1 : 1);
        return 1;
    }
    if (!strncmp(a, "bp-slide:", 9)) {
        treadmill_slide(strike_screen_root(STRIKE_SCREEN_BATTLEPASS), &TM_BP, atoi(a + 9) < 0 ? -1 : 1);
        return 1;
    }
    if (!strncmp(a, "bp-claim:", 9)) {
        bp_claim(atoi(a + 9));
        return 1;
    }
    if (!strcmp(a, "bp-claim-all")) { // claimMassItem(A): the first claimable card
        const BpScan scan = bp_scan(battlepass_ui_pass(), battlepass_ui_bpp());
        if (scan.first) bp_claim(scan.first);
        return 1;
    }
    if (!strcmp(a, "track-claim")) { // gn.claimSlots(l): every claimable slot, one after another (Fy)
        if (TQ.busy) return 1;
        const char *slots[64];
        const int n = econ_on() ? strike_econ_track_claimable(slots, 64) : 0;
        TQ.n = TQ.i = 0;
        for (int i = 0; i < n; i++) snprintf(TQ.slots[TQ.n++], sizeof(TQ.slots[0]), "%s", slots[i]);
        if (!econ_on()) KN_LOG_DEBUG1("[strike] Rewards Track: claiming needs a signed-in Strike account\n");
        else KN_LOG_VERBOSE2("[strike] Rewards Track: claiming %d level(s)\n", n);
        if (TQ.n > 0) {
            TQ.busy = 1;
            track_claim_next();
        }
        return 1;
    }
    if (!strncmp(a, "mission-claim:", 14)) {
        if (!strike_econ_claim_mission(a + 14, on_claimed, (void *) 1)) printf("[strike] mission claim: no Strike account\n");
        return 1;
    }
    return 0;
}

void strike_meta_econ_changed(void) {
    if (strike_screen_root(STRIKE_SCREEN_PROFILE)) strike_screen_rerender(STRIKE_SCREEN_PROFILE); // claims, level
    if (!econ_on()) { // signed out: nothing of the old account stays
        cJSON_Delete(T.econ_missions);
        T.econ_missions = NULL;
        cJSON_Delete(T.econ_cards);
        T.econ_cards = NULL;
        T.econ_missions_asked = 0;
        T.econ_seq++;
    }
}

void strike_meta_tick(Client *client) {
    T.client = client;
    // a Strike account's missions request with no answer by the Battle Pass's deadline has failed: the tab says so, with Retry
    if (T.econ_missions_asked && !T.econ_missions && !T.econ_missions_failed && (int64_t) time(NULL) - T.econ_missions_asked_s >= BATTLEPASS_LOAD_TIMEOUT_S) {
        printf("[strike] missions: no answer in %d s\n", BATTLEPASS_LOAD_TIMEOUT_S);
        T.econ_missions_failed = 1;
    }
    // the Rewards tab follows the Battle Pass data (bpdl / bppd / bpclmr), the Missions tab
    // account.activeMissions (Mithril redraws on every change)
    const HtmlElem root = strike_screen_root(STRIKE_SCREEN_BATTLEPASS);
    if (root) {
        if (T.bp_tab == 0 && rewards_signature() != T.rewards_sig) bp_tab(root, 0);
        else if (T.bp_tab == 1 && missions_signature() != T.missions_sig) bp_tab(root, 1);
    }
    // the daily missions widgets follow the same data; a Strike account's missions are asked for
    // here too (the Missions tab may never open), once per session
    if (econ_on()) econ_missions_ask();
    const unsigned long sig = missions_signature();
    if (sig != T.widget_sig) {
        T.widget_sig = sig;
        if (!econ_on() && bp_has_data()) account_challenges_request(); // account.activeMissions: asked once per session
        const HtmlElem menu = strike_screen_root(STRIKE_SCREEN_MAIN_MENU);
        if (menu) widget_fill_main(menu);
        const HtmlElem tap = strike_screen_root(STRIKE_SCREEN_TAP_TO_PLAY);
        if (tap) widget_fill_tap(tap);
    }
}

// state.strike (strike_ui_scenario): "profileTab" (PROFILE-DETAILS: 0 / 1), "logoutConfirm" (the Logout
// confirm), "tapMissions" (TAP_TO_PLAY's Missions panel is up), "progression" (the game config's
// accountProgression:
// {"<level>": {slotId, items: [{type, currency, amount} ...], isPlayerChoice, bundleName}}),
// "claimed" (progressionClaimedItems: slot ids), "reward" (REWARD_TO_CLAIM's {type, currency,
// amount}), "battlepassTab" (the Battle Pass tab), "matchDetails" (true: the podium button)
void strike_meta_scenario(Client *client, const cJSON *st) {
    T.client = client;
    const cJSON *prog = cJSON_GetObjectItemCaseSensitive(st, "progression");
    const cJSON *claimed = cJSON_GetObjectItemCaseSensitive(st, "claimed");
    const cJSON *reward = cJSON_GetObjectItemCaseSensitive(st, "reward");
    if (cJSON_IsObject(prog)) {
        cJSON_Delete(T.progression);
        T.progression = cJSON_Duplicate(prog, 1);
        cJSON_Delete(T.claimed);
        T.claimed = cJSON_IsArray(claimed) ? cJSON_Duplicate(claimed, 1) : NULL;
        strike_screen_rerender(STRIKE_SCREEN_PROFILE);
    }
    if (cJSON_IsObject(reward)) {
        cJSON_Delete(T.reward);
        T.reward = cJSON_Duplicate(reward, 1);
        strike_screen_rerender(STRIKE_SCREEN_REWARD_TO_CLAIM);
    }
    const cJSON *tab = cJSON_GetObjectItemCaseSensitive(st, "battlepassTab");
    if (cJSON_IsNumber(tab)) {
        char a[24];
        snprintf(a, sizeof(a), "bp-tab:%d", tab->valueint);
        strike_meta_action(a);
    }
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(st, "tapMissions"))) T.tap_missions = 1; // the panel up (showMissions)
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(st, "matchDetails"))) strike_menu_action("match-details");
    const cJSON *ptab = cJSON_GetObjectItemCaseSensitive(st, "profileTab"); // PROFILE-DETAILS' tab (strike_profile.c)
    if (cJSON_IsNumber(ptab)) {
        char a[24];
        snprintf(a, sizeof(a), "profile-tab:%d", ptab->valueint);
        strike_profile_action(a);
    }
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(st, "logoutConfirm"))) strike_profile_action("logout");
}
