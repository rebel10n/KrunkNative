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
//                           yF / bF per team), Close = kt(). TA (Report players) is never reset.
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
//   legacyData the END table (hud_modes_end_rows / _cell: the mode's endStats, the server's endSort
//              order, team totals), else the players by score
#include <strike_ui.h>
#include <html_ui.h>
#include <ui_logic.h>
#include <backend_ui.h>
#include <hud_modes.h>
#include <spectate.h>
#include <modes.h>
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
    int report;                 // TA: Report players was pressed (the board shows report symbols)
    cJSON *progression;         // the game config's accountProgression (owned; NULL = none)
    cJSON *claimed;             // account.progressionClaimedItems (owned)
    cJSON *reward;              // REWARD_TO_CLAIM's reward (owned)
    // kn_brain's Strike missions (GET /strike/v1/missions/me) as UD's cards read them (owned)
    cJSON *econ_missions;       // the answer
    cJSON *econ_cards;          // its daily missions in the challenge shape mission_card() reads
    int econ_missions_asked;    // a request is out / answered for the current state
    unsigned econ_seq;          // bumped on every answer (the Missions tab re-fills)
    int track_asked;            // the game config (accountProgression) was asked
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
        return;
    }
    cJSON_Delete(T.econ_missions);
    T.econ_missions = cJSON_Duplicate(data, 1);
    econ_cards_build();
    T.econ_seq++;
    printf("[strike] missions: %d daily%s\n", cJSON_GetArraySize(T.econ_cards),
           cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "locked")) ? " (locked)" : "");
}

static void econ_missions_ask(void) {
    if (T.econ_missions_asked || !econ_on()) return;
    T.econ_missions_asked = strike_econ_get_missions(on_missions, NULL);
}

// Se.bpData && account.BP; a Strike account: its missions answer, unlocked
static int bp_has_data(void) {
    if (econ_on()) return T.econ_missions && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(T.econ_missions, "locked"));
    return account_get() && battlepass_ui_pass();
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
    unsigned long sig = (unsigned long) (uintptr_t) list * 31u + (unsigned long) bp_has_data() + T.econ_seq * 977u;
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

// FT: the tab selector's .selected and the tab's content (g(UD) or g(I2, {screen}))
static void bp_tab(const HtmlElem root, const int tab) {
    T.bp_tab = tab;
    HtmlElem items[4];
    const int n = html_dom_query_all(root, ".tabSelectorItem", items, 4);
    for (int i = 0; i < n; i++) html_dom_set_class(items[i], "selected", i == tab);
    const HtmlElem content = html_dom_query(root, ".tabsContent");
    if (!content) return;
    char *html = NULL;
    if (tab == 1 && !T.econ_missions_asked) econ_missions_ask(); // a Strike account: GET /strike/v1/missions/me
    if (tab == 1 && bp_has_data()) {
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
        printf("[strike] MISSION_TAB_SCREEN %d daily missions\n", count);
    } else {
        // I2: "Loading Rewards" / "Loading Missions" (the Rewards tab's OD is not ported: with bpData
        // it stays at I2 as before)
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
}

static void bp_render(const HtmlElem root) {
    HtmlElem items[4];
    const int n = html_dom_query_all(root, ".tabSelectorItem", items, 4);
    char hook[48];
    for (int i = 0; i < n; i++) {
        snprintf(hook, sizeof(hook), "strikeAction('bp-tab:%d')", i);
        html_dom_set_attr(items[i], "onclick", hook);
    }
    bp_tab(root, 0); // a new FT: viewState.selectedTab 0
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
    printf("[strike] MATCH_DETAILS_SCREEN %s, %d players, %d stat columns\n", ffa ? "ffa" : "teams", rn, sn);
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
    if (status == 200) printf("[strike] claimed %s\n", user ? "a mission" : "a track level");
    else printf("[strike] claim refused (%d): %s\n", status, msg ? msg : "no answer");
    if (user) { // a mission: its list again
        T.econ_missions_asked = 0;
        econ_missions_ask();
    }
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
    printf("[strike] PROGRESSION_TRACK %d levels, level %d, %d claimable\n", max, level, claimable);
    free(html);
    free(claim);
    free(columns);
}

// KU.slide(n): one page of columns left / right; the arrows' --available follow
static void track_slide(const int dir) {
    const HtmlElem profile = strike_screen_root(STRIKE_SCREEN_PROFILE);
    const HtmlElem cols = profile ? html_dom_query(profile, ".frvr-progression-track-rewards__treadmill-columns") : 0;
    if (!cols) return;
    HtmlScroll sc = {0};
    if (!html_dom_get_scroll(cols, &sc)) return;
    HtmlElem kids[256];
    const int n = html_dom_query_all(cols, ".frvr-progression-track-rewards__column", kids, 256);
    HtmlRect first = {0};
    if (n > 0) html_dom_get_rect(kids[0], &first);
    const float r = (float) (n - 1) * first.w - sc.client_w;
    const float l = sc.left + (float) dir * sc.client_w, u = sc.client_w + sc.left;
    const int left_ok = !(l <= 0.0f && dir == -1), right_ok = !(u >= r && dir == 1);
    html_dom_set_scroll(cols, l, sc.top);
    HtmlElem buttons[2];
    if (html_dom_query_all(profile, ".frvr-progression-track-rewards__treadmill-button", buttons, 2) == 2) {
        html_dom_set_class(buttons[0], "frvr-progression-track-rewards__treadmill-button--available", left_ok);
        html_dom_set_class(buttons[1], "frvr-progression-track-rewards__treadmill-button--available", right_ok);
    }
}

// ---------------------------------------------------------------- hooks

void strike_meta_render(const StrikeScreen s, const HtmlElem root) {
    if (!root) return;
    switch (s) {
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
    if (!strncmp(a, "bp-tab:", 7)) {
        const HtmlElem root = strike_screen_root(STRIKE_SCREEN_BATTLEPASS);
        const int t = atoi(a + 7);
        if (t == 1) T.econ_missions_asked = 0; // the Missions tab asks for the account's missions on every visit
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
    if (!strncmp(a, "match-details-report-player:", 28)) {
        printf("[strike] report player %s: the report popup (Hy) is not ported\n", a + 28);
        return 1;
    }
    if (!strncmp(a, "track-slide:", 12)) {
        track_slide(atoi(a + 12) < 0 ? -1 : 1);
        return 1;
    }
    if (!strcmp(a, "track-claim")) { // gn.claimSlots(l): every claimable slot (a pick-one slot: its first reward)
        const char *slots[64];
        const int n = econ_on() ? strike_econ_track_claimable(slots, 64) : 0;
        for (int i = 0; i < n; i++) strike_econ_claim_level(slots[i], 0, on_claimed, NULL);
        if (!econ_on()) printf("[strike] Rewards Track: claiming needs a signed-in Strike account\n");
        else printf("[strike] Rewards Track: claiming %d level(s)\n", n);
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
    // the Missions tab follows account.activeMissions (Mithril redraws on every change)
    if (T.bp_tab == 1) {
        const HtmlElem root = strike_screen_root(STRIKE_SCREEN_BATTLEPASS);
        if (root && missions_signature() != T.missions_sig) bp_tab(root, 1);
    }
}

// state.strike (strike_ui_scenario): "progression" (the game config's accountProgression:
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
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(st, "matchDetails"))) strike_menu_action("match-details");
}
