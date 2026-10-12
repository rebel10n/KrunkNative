// Krunker v7.1.4 Battle Pass: windows[5] (showWindow(6), label "battlepass"; src/index.js:5749),
// its popups (purchasePop, skipLevelPop, skippedLevelPop, purchasedPop), the global
// selectBPTier / skipLevels, the io handlers (bpdl io_dataLoaded, bppd io_syncBpProg, bpclmr
// claimItemRes, purbp purchaseRes, purbpl purchaseLvlRes) and the end screen's Battle Pass
// section (src/ui/game.js endScreen.tabs.performance, #endBPLayout). Data from kn_brain's
// social socket (services/brain/season.c); the rules are shared/src/season.c.
//
// KrunkNative differences (documented):
// - The VIP tier and skipped levels are paid in KR from the account's wallet (kn_brain
//   kr_transactions), not in USD through Xsolla / Steam: the price buttons read "<n> KR".
// - "CLAIM" on a free card for a player without the VIP tier (v7: freeclaimItem behind a
//   rewarded video ad) claims at once: KrunkNative shows no ads.
// - The Challenges tab lists the daily challenges like v7 and then KrunkNative's weekly ones
//   (v7 has none; they used to sit in the Challenges window).
// - v7 bug fixed: the window header is i18n "windows.battlepass.header", a key en.js lacks, so
//   v7 shows "UNLOCALIZED STRING"; KrunkNative uses en.js "battlepass.header" ("Battle Pass").
// - The end screen's tmpBPData arrives from kn_brain after the stats commit (["bpEnd", data])
//   instead of with the "end" packet: KrunkNative's game server does not hold the pass.
#include <backend.h>
#include <backend_ui.h>
#include <html_ui.h>
#include <ui_logic.h>
#include <season.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../viewer/viewer_store.h"

static cJSON *g_bp_json;      // windows[5].data (the "bpdl" payload), NULL none
static BattlePass *g_bp;      // its parse (bp_parse_items)
static int g_bp_status;       // windows[5].dataStatus: 0 not asked, 1 loading, 2 answered
static int g_has_progress;    // account.BP set (bppd)
static int64_t g_bpp;         // account.BP.bpp
static int g_tier;            // account.BP.tier
static char g_claimed[4096];  // account.BP.items_claimed
static int g_tab;             // windows[5].tabIndex
static int g_lvl;             // windows[5].battlepassLvl
static int g_after_purchase;  // windows[5].afterData = purchasePop
static cJSON *g_end;          // GUI.endScreen bpData (tmpBPData of the last match), NULL none
static int64_t g_now_override; // --render-scenario: a fixed Date.now() (unix s), 0 = the clock

#define BP_WINDOW 6
#define BP_TAB_HOME "<span class='material-icons' style='color:#fbc02d;font-size:42px;vertical-align:bottom'>home</span>"
static const char *const BP_TABS[2] = {BP_TAB_HOME, "Challenges"}; // config.bpTabs

static double jnum(const cJSON *o, const char *k) {
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
    return cJSON_IsNumber(v) ? v->valuedouble : cJSON_IsString(v) && v->valuestring ? strtod(v->valuestring, NULL) : 0.0;
}

static const char *jstr(const cJSON *o, const char *k) {
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

// new Date(iso).getTime() / 1000 of "YYYY-MM-DDTHH:MM:SS(.mmm)Z"
static int64_t iso_seconds(const char *s) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) < 3) return 0;
    // days from civil (Howard Hinnant)
    y -= mo <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = era * 146097 + doe - 719468;
    return days * 86400 + h * 3600 + mi * 60 + se;
}

void battlepass_ui_reset(void) {
    cJSON_Delete(g_bp_json);
    g_bp_json = NULL;
    free(g_bp);
    g_bp = NULL;
    g_bp_status = 0;
    g_has_progress = 0;
    g_bpp = 0;
    g_tier = 0;
    g_claimed[0] = 0;
    g_tab = 0;
    g_lvl = 0;
    g_after_purchase = 0;
    cJSON_Delete(g_end);
    g_end = NULL;
    g_now_override = 0;
}

static void set_data(const cJSON *bp) {
    cJSON_Delete(g_bp_json);
    g_bp_json = cJSON_IsObject(bp) ? cJSON_Duplicate(bp, 1) : NULL;
    free(g_bp);
    g_bp = NULL;
    if (!g_bp_json) return;
    g_bp = calloc(1, sizeof *g_bp);
    if (!g_bp) return;
    g_bp->id = (int) jnum(g_bp_json, "id");
    g_bp->season = (int) jnum(g_bp_json, "season");
    snprintf(g_bp->name, sizeof g_bp->name, "%s", jstr(g_bp_json, "name"));
    g_bp->bpp_per_level = (int) jnum(g_bp_json, "bpp_perlevel");
    g_bp->item_count = (int) jnum(g_bp_json, "itemcount");
    g_bp->tier1_price_kr = (int) jnum(g_bp_json, "tier1price");
    g_bp->skip_price_kr = (int) jnum(g_bp_json, "skipprice");
    snprintf(g_bp->tier1_name, sizeof g_bp->tier1_name, "%s", jstr(g_bp_json, "tier1name")[0] ? jstr(g_bp_json, "tier1name") : "VIP");
    g_bp->start_s = iso_seconds(jstr(g_bp_json, "startdate"));
    g_bp->end_s = iso_seconds(jstr(g_bp_json, "enddate"));
    bp_parse_items(g_bp, jstr(g_bp_json, "itemdata"));
}

static void set_progress(const cJSON *p) {
    if (!cJSON_IsObject(p)) return;
    g_has_progress = 1;
    g_bpp = (int64_t) jnum(p, "bpp");
    g_tier = (int) jnum(p, "tier");
    snprintf(g_claimed, sizeof g_claimed, "%s", jstr(p, "items_claimed"));
}

int battlepass_ui_tier(void) { return g_has_progress ? g_tier : 0; }

static void request_data(void) {
    if (g_bp_status) return;
    g_bp_status = 1;
    backend_social_send_text("[\"bpdl\"]"); // io.send("bpdl")
}

// ------------------------------------------------------------------ windows[5].gen

static void gen_tabs(StrBuf *sb) {
    sb_append(sb, "<div class=\"bpBG\"></div>\n\t\t\t<div style=\"height:15px;\"></div>\n\t\t\t<div class=\"bigMenTabs\">");
    for (int i = 0; i < 2; i++) {
        sb_appendf(sb, "<div class=\"bigMenTab flex%s\"\n\t\t\t\tonclick=\"playSelect();windows[5].switchTab(%d)\"\n\t\t\t\tonmouseenter=\"SOUND.play('tick_0',0.1)\"\n\t\t\t\tstyle=\"font-size:16px\">%s</div>",
                   g_tab == i ? " bigMenTabA" : "", i, BP_TABS[i]);
    }
    sb_append(sb, "</div>");
}

static void gen_card(StrBuf *sb, const int track, const int col, const int completed, const int vip) {
    const BattlePassItem *it = bp_item_at(g_bp, track, col);
    const int free_track = track == 0;
    const int id = it ? it->id : -1;
    const int claimed = it && bp_is_claimed(g_claimed, id);
    const int claimable = (free_track || vip) && it && !claimed;
    const int ad = free_track && !vip; // freeclaimItem (rewarded ad in v7)
    const ViewerSkin *skin = it && it->skin > 0 ? viewer_skin(it->skin) : NULL;
    sb_appendf(sb, "<div class=\"bpCard %s%s\">", free_track ? "free" : "vip", completed ? " completed" : "");
    if (skin) {
        char thumb[256];
        store_skin_preview(it->skin, thumb, sizeof thumb);
        const int r = skin->rarity >= 0 && skin->rarity < g_viewer_rarity_count ? skin->rarity : 0;
        sb_appendf(sb, "<div class=\"bpCardSlot\" onmouseenter=\"playTick(0.1);\" onclick=\"playSelect(0.1);viewItem(%d);\">\n\t\t\t\t\t\t\t%s\n\t\t\t\t\t\t\t<div class=\"bpCardImgH\">\n\t\t\t\t\t\t\t\t<img class=\"bpCardImg\" loading=\"lazy\" draggable=\"false\" src=\"%s\" style=\"filter:drop-shadow(0 0 0.2rem %s88)\">\n\t\t\t\t\t\t\t</div>\n\t\t\t\t\t\t</div>\n\n\t\t\t\t\t\t\n\t\t\t\t\t\t",
                   it->skin, vip || free_track ? "" : "<div class=\"bpCardLock\"></div>", thumb, g_viewer_rarities[r].color);
        if (completed && claimable)
            sb_appendf(sb, "<div class='bpClaimB' onclick=\"windows[5].%sclaimItem(%d)\"> %sCLAIM</div>", ad ? "free" : "", id,
                       ad ? "<img style='vertical-align: bottom;margin-right:10px;display:inline' height='30px' src ='./img/frvr-ui/ad-icon.svg' class='frvr-button__icon'/>" : "");
        sb_append(sb, "\n\n\t\t\t\t\t\t");
        if (claimed) sb_append(sb, "<div class=\"bpClaimed\"><span class=\"material-icons\" style=\"font-size:160px;color:#38cd3a;line-height:256px\">done</span></div>");
        sb_append(sb, "\n\n\t\t\t\t\t\t");
    }
    sb_append(sb, "</div>");
}

static void gen_home(StrBuf *sb) {
    const int levels = g_bp->item_count;
    const int64_t max_bpp = bp_max_bpp(g_bp);
    int complete = 0;
    int64_t points = 0;
    g_lvl = bp_level(g_bp, g_bpp, &points, &complete);
    const int vip = g_tier >= 1;
    const int64_t now = g_now_override ? g_now_override : (int64_t) time(NULL);
    const int64_t cap = bp_cap_total(g_bp, now);
    const int64_t avail = bp_available(g_bp, g_bpp, now);
    sb_append(sb, "<div style=\"display: inline-block;position: relative;\">");
    sb_append(sb, "<div class=\"bpTopH\">\n\t\t\t\t<div class=\"bpSeasonH\">\n\t\t\t\t\t");
    sb_append_esc(sb, g_bp->name);
    sb_append(sb, "\n\t\t\t\t</div>");
    if (complete) {
        sb_append(sb, "<div class=\"bpLevelH\">\n\t\t\t\t\t<div class=\"bpLevelText\">Battlepass Complete</div>\n\t\t\t\t</div>");
    } else {
        sb_appendf(sb, "<div class=\"bpLevelH\">\n\t\t\t\t\t<div class=\"bpLevelText\">Level %d/%d</div>\n\t\t\t\t\t<div class=\"bpXPText\">\n\t\t\t\t\t\t<span style='color:#fbc02d'>BPP</span> %lld/%d\n\t\t\t\t\t\t<div class=\"bpXPBarH\">\n\t\t\t\t\t\t\t<div class=\"bpXPBar\" style=\"width:%.15g%%;\"></div>\n\t\t\t\t\t\t</div>\n\t\t\t\t\t</div>",
                   g_lvl, levels, (long long) points, g_bp->bpp_per_level, (double) points / g_bp->bpp_per_level * 100.0);
        sb_appendf(sb, "\n\t\t\t\t\t<div class=\"bpXPText\">\n\t\t\t\t\t\tAvailable BPP <img class=\"bpXPIcon\" onmouseenter=\"windows[5].showBalloon()\" onmouseleave=\"windows[5].hideBalloon()\" src=\"./img/frvr-ui/info-icon.svg\">\n\t\t\t\t\t\t<div class=\"bpXPBarH\">\n\t\t\t\t\t\t\t<div class=\"bpXPBar\" style=\"width:%.15g%%;\"></div>\n\t\t\t\t\t\t</div>\n\t\t\t\t\t</div>",
                   cap > 0 ? (double) avail / (double) cap * 100.0 : 0.0);
        sb_appendf(sb, "\n\t\t\t\t\t<div class=\"bpInfoBalloon\">\n\t\t\t\t\t\t<div>BPP has a daily cap. The amount you can earn increases every day.</div>\n\t\t\t\t\t\t<div style=\"margin-top: 20px\">Current available BPP: <span style='color:#00b1ff'>%lld</span></div>\n\t\t\t\t\t</div>\n\t\t\t\t</div>",
                   (long long) avail);
    }
    sb_append(sb, "</div>");
    sb_append(sb, "<div class=\"bpMidH\">");
    sb_appendf(sb, "<div class=\"bpRowTitles\">\n\t\t\t\t<div class=\"bpCard free\">\n\t\t\t\t\t<div class=\"bpRowTitleText\">Free</div>\n\t\t\t\t</div>\n\t\t\t\t<div class=\"bpCard vip active\">\n\t\t\t\t\t%s\n\t\t\t\t\t<div class=\"bpRowTitleText\" style=\"left: 19%%;\">VIP</div>\n\t\t\t\t</div>\n\t\t\t</div>",
               vip ? "" : "<div class=\"bpCardLock\"></div>");
    sb_append(sb, "<div class=\"bSliderH\">\n\t\t\t\t<div class=\"bSliderBtn disabled\" onmouseenter=\"playTick(0.1);\" onmousedown=\"playSelect(0.1);windows[5].updateScroll(-1);\">\n\t\t\t\t\t<div class=\"bpSliderArrow\"></div>\n\t\t\t\t</div>\n\t\t\t</div>");
    sb_append(sb, "<div class=\"bpCols\" onwheel=\"windows[5].updateScrollWheel();\">");
    for (int col = 0; col < levels; col++) {
        const int level = col + 1;
        const int done = g_lvl >= level, active = g_lvl == level;
        sb_appendf(sb, "<div class=\"bpCol\" style='%s'>\n\t\t\t\t\t<div class=\"bpLevelNum%s\">%d</div>\n\t\t\t\t\t", done ? "" : "filter:grayscale(100%)",
                   active ? " active" : done ? " completed" : "", level);
        for (int track = 0; track < BP_TRACKS; track++) gen_card(sb, track, col, done, vip);
        sb_append(sb, "\n\t\t\t\t</div>");
    }
    sb_append(sb, "</div>");
    sb_append(sb, "<div class=\"bSliderH\">\n\t\t\t\t<div class=\"bSliderBtn\" onmouseenter=\"playTick(0.1);\" onmousedown=\"playSelect(0.1);windows[5].updateScroll(1);\">\n\t\t\t\t\t<div class=\"bpSliderArrow right\"></div>\n\t\t\t\t</div>\n\t\t\t</div>");
    sb_append(sb, "</div>");
    sb_appendf(sb, "<div class=\"bpBotH\">\n\t\t\t\t<div class=\"bpBtn claim%s\" onmouseenter=\"playTick(0.1);\" onclick=\"playSelect(0.1);windows[5].purchasePop();\">Purchase VIP Tier</div>\n\t\t\t\t<div class=\"bpBtn skip%s\" onmouseenter=\"playTick(0.1);\" onclick=\"playSelect(0.1);windows[5].skipLevelPop()\">Skip levels</div>\n\t\t\t</div>",
               vip ? " blockClick" : "", g_bpp >= max_bpp ? " blockClick" : "");
    sb_append(sb, "</div>");
}

// windows[5].genDailyCard(challenge, marginTop)
static void gen_daily_card(StrBuf *sb, const cJSON *c, const int margin) {
    const cJSON *dat = cJSON_GetObjectItemCaseSensitive(c, "DAT"), *rew = cJSON_GetObjectItemCaseSensitive(c, "REW");
    const int boss = jnum(dat, "boss") != 0.0;
    const int done = jnum(c, "PT") != 0.0;
    const int time_key = !strcmp(jstr(dat, "ky"), "time");
    const double val = jnum(dat, "val"), pv = jnum(c, "PV");
    char icon[256], path[64], name[160], text[64];
    snprintf(path, sizeof path, "img/chals/icon_%d.png", (int) jnum(c, "IC"));
    ui_img_url(path, icon, sizeof icon); // UTILS.assetsUrl: KrunkNative's ui/ copy (fetch_season_images.sh)
    account_challenge_name(c, name, sizeof name);
    sb_appendf(sb, "<div class='chalCard' style='margin-top:%dpx;%s'><img class='chalImgC' src='%s'  style='%s%s' />", margin,
               boss ? "box-shadow: 0 0 14px #ffa000;background:rgb(79 50 0 / 40%);border:4px solid #ff9c00;" : "", icon,
               boss ? "border: 4px solid #ff9c00;" : "", done ? "filter:grayscale(1);brightness(0.8);" : "");
    if (done) sb_append(sb, "<div class='challComplB'><span class='material-icons' style='font-size:110px;color:#38cd3a;'>done</span></div>");
    sb_append(sb, "<div class='chalRight'>");
    sb_append_esc(sb, name);
    if (jnum(c, "TR") > 0) {
        account_readable_time(text, sizeof text, jnum(c, "TR"));
        sb_appendf(sb, "<div class='chalExpr'>%s left</div>", text);
    }
    if (done) snprintf(text, sizeof text, "Completed");
    else if (time_key) account_readable_time(text, sizeof text, val);
    else snprintf(text, sizeof text, "%.15g/%.15g", pv, val);
    sb_appendf(sb, "<div class='chalXPBar'><div class='chalXPVal'>%s</div><div class='chalXPBarC' style='width:%.15g%%;'></div></div></div>", text,
               done ? 100.0 : time_key || val == 0.0 ? 0.0 : pv / val * 100.0);
    const double bpp = jnum(rew, "bpp"), kr = jnum(rew, "kr"), jnk = jnum(rew, "jnk");
    sb_appendf(sb, "<div class='chalRews' style='%s'>", boss ? "border-left: 3px solid #ff9c00;" : "");
    sb_appendf(sb, "<div class='chalRewV' style='%s'><span style='color:white'>+</span>%.15g <span style='color:#fbc02d'>BPP</span></div>", bpp ? "" : "filter:grayscale(1);", bpp);
    sb_appendf(sb, "<div class='chalRewV' style='%s'><span style='color:white'>+</span>%.15g <span style='color:#fbc02d'>KR</span></div>", kr ? "" : "filter:grayscale(1);", kr);
    sb_appendf(sb, "<div class='chalRewV' style='%s'><span style='color:white'>+</span>%.15g <span class='material-icons' style='color:#cc51f8;font-size:29px;vertical-align:top;'>plumbing</span></div>",
               jnk ? "" : "filter:grayscale(1);", jnk);
    // REW.sk: every reward skin (an array or "a,b,c")
    const cJSON *sk = cJSON_GetObjectItemCaseSensitive(rew, "sk");
    int skins[16], n = 0;
    if (cJSON_IsNumber(sk)) skins[n++] = sk->valueint;
    else if (cJSON_IsArray(sk)) {
        const cJSON *it;
        cJSON_ArrayForEach(it, sk) if (n < 16) skins[n++] = cJSON_IsNumber(it) ? it->valueint : atoi(cJSON_IsString(it) ? it->valuestring : "-1");
    } else if (cJSON_IsString(sk)) {
        for (const char *p = sk->valuestring; p && *p && n < 16;) {
            skins[n++] = atoi(p);
            p = strchr(p, ',');
            if (p) p++;
        }
    }
    for (int i = 0; i < n; i++) {
        const ViewerSkin *skin = skins[i] > 0 ? viewer_skin(skins[i]) : NULL;
        if (!skin) continue;
        char url[256];
        store_skin_preview(skins[i], url, sizeof url);
        const int r = skin->rarity >= 0 && skin->rarity < g_viewer_rarity_count ? skin->rarity : 0;
        sb_appendf(sb, "<div class='chalRewSkn' style='background-image:url(%s);border:2px solid %s;' onclick='viewItem(%d)'></div>", url,
                   g_viewer_rarities[r].color, skins[i]);
    }
    sb_append(sb, "</div></div>");
}

// The daily (and KrunkNative's weekly) cards, PT descending (v7's sort)
static int gen_rotating(StrBuf *sb, const cJSON *list, const char *type) {
    const cJSON *rows[64];
    int n = 0;
    const cJSON *c;
    cJSON_ArrayForEach(c, list) {
        if (n < 64 && !strcmp(jstr(c, "T"), type)) rows[n++] = c;
    }
    for (int i = 1; i < n; i++) { // stable: sort((a, b) => b.PT - a.PT)
        const cJSON *x = rows[i];
        int j = i - 1;
        while (j >= 0 && jnum(rows[j], "PT") < jnum(x, "PT")) {
            rows[j + 1] = rows[j];
            j--;
        }
        rows[j + 1] = x;
    }
    for (int i = 0; i < n; i++) gen_daily_card(sb, rows[i], i ? 20 : 0);
    return n;
}

static void gen_challenges(StrBuf *sb) {
    const cJSON *list = account_challenges();
    sb_append(sb, "<div class='challCrdMsg' style='margin-bottom:25px;font-size:27px;color:#9f9f9f;position: relative;'>Abandoning a match will result in the loss of challenge progress.</div>");
    sb_append(sb, "<div style='margin-top:10px;'>");
    if (!gen_rotating(sb, list, "daily")) sb_append(sb, "<div class='challCrdMsg'>Check back later for new daily battle pass challenges!</div>");
    sb_append(sb, "</div>");
    int weekly = 0;
    const cJSON *c;
    cJSON_ArrayForEach(c, list) weekly += !strcmp(jstr(c, "T"), "weekly");
    if (weekly) { // KrunkNative: the weekly challenges (v7 has none)
        sb_append(sb, "<div class='challCrdMsg' style='margin-top:40px;margin-bottom:25px;font-size:27px;color:#9f9f9f;position: relative;'>Weekly Challenges</div>");
        sb_append(sb, "<div style='margin-top:10px;'>");
        gen_rotating(sb, list, "weekly");
        sb_append(sb, "</div>");
    }
}

static char *gen_window(void) {
    StrBuf sb = {0};
    gen_tabs(&sb);
    if (!g_bp || !g_has_progress) {
        request_data();
        sb_appendf(&sb, "<div style=\"font-size: 40px;text-align: center;display: inline-block;position: relative;width: 100%%;\">\n\t\t\t\t%s\n\t\t\t</div>",
                   g_bp_status == 2 && g_has_progress ? "No Battle Pass Found" : "Loading Battle Pass");
    } else if (g_tab == 0) {
        gen_home(&sb);
    } else {
        if (!account_challenges()) account_challenges_request();
        gen_challenges(&sb);
    }
    return sb_finish(&sb);
}

char *battlepass_window_gen(const int id, const char **header) {
    if (id != BP_WINDOW || !account_get()) return NULL; // guests: the captured "Sign-in" card
    if (header) *header = "Battle Pass"; // v7 bug fixed: i18n windows.battlepass.header is missing
    return gen_window();
}

// windows[5].updateScroll(dir, columns)
static void update_scroll(const int dir, const int columns) {
    const HtmlElem cols = html_dom_query(0, ".bpCols");
    HtmlScroll s;
    if (!cols || !html_dom_get_scroll(cols, &s)) return;
    const HtmlElem first = html_dom_first_child(cols);
    const float col_w = first ? html_dom_offset_width(first) : 0.0f; // children[0].scrollWidth
    const float max_left = (float) (html_dom_child_count(cols) - 1) * col_w - s.client_w;
    float left = s.left + (float) dir * (col_w * (float) (columns ? columns : 8)); // columnsToScroll
    const float right = s.client_w + s.left;
    const int can_left = !(left <= 0.0f) || dir != -1;
    const int can_right = !(right >= max_left) || dir != 1;
    if (left < 0.0f) left = 0.0f;
    html_dom_set_scroll(cols, left, s.top);
    HtmlElem btns[2];
    if (html_dom_query_all(0, ".bSliderBtn", btns, 2) == 2) {
        html_dom_set_class(btns[0], "disabled", !can_left);
        html_dom_set_class(btns[1], "disabled", !can_right);
    }
}

void battlepass_window_post(const int id) {
    if (id != BP_WINDOW || !account_get()) return;
    const HtmlElem mw = html_dom_by_id("menuWindow");
    if (mw) html_dom_set_style(mw, "width", g_tab ? "1200px" : "1580px"); // this.width = tabIndex ? 1200 : 1580
    if (g_tab == 0 && g_bp && g_lvl - 3 > 0) update_scroll(1, g_lvl - 3);
}

// ------------------------------------------------------------------ popups

static void generic_popup(const char *html, const char *cls) {
    ui_loadout_clear_pops();
    HtmlElem holder = html_dom_by_id("popupHolder");
    if (holder) html_dom_set_display(holder, "block");
    HtmlElem pop = html_dom_by_id("genericPop");
    if (!pop) return;
    html_dom_set_attr(pop, "class", cls);
    html_dom_set_attr(pop, "style", "");
    html_dom_set_html(pop, html);
    html_dom_set_display(pop, "block");
}

static void price_text(char *out, const size_t n, const int64_t kr) { snprintf(out, n, "%lld KR", (long long) kr); }

// windows[5].purchasePop()
static void purchase_pop(void) {
    if (!g_bp) {
        g_after_purchase = 1;
        menu_show_window(BP_WINDOW, 0);
        return;
    }
    char code[128] = "", price[32];
    html_storage_get("krk_ccKN", code, sizeof code); // the Store's saved creator code
    price_text(price, sizeof price, g_bp->tier1_price_kr);
    StrBuf sb = {0};
    sb_append(&sb, "<div class=\"bp-generic-img-holder\">\n\t\t\t<div class=\"bp-generic-img-back\"></div>\n\t\t\t<div class=\"bp-generic-img\"></div>\n\t\t</div>\n\t\t<div class=\"bp-generic-text\">\n\t\t\t<div class=\"bp-generic-header\" style=\"margin-bottom: 25px;\">Purchase ");
    for (const char *p = g_bp->tier1_name; *p; p++) sb_appendf(&sb, "%c", (*p >= 'a' && *p <= 'z') ? *p - 32 : *p);
    sb_append(&sb, " Battle Pass</div>\n\t\t\t<div class=\"bp-generic-info\">Unlock ");
    for (const char *p = g_bp->tier1_name; *p; p++) sb_appendf(&sb, "%c", (*p >= 'A' && *p <= 'Z') ? *p + 32 : *p);
    sb_append(&sb, " rewards\n\t\t\t\t<div class='battlepassCreatorH'><div class='battlepassCreatorText'>Support your favorite Content Creator by using a Creator Code</div><div><input type='text' id='battlepassCreatorCode' class='smlInput' ");
    if (code[0]) {
        sb_append(&sb, "value='");
        sb_append_esc(&sb, code);
        sb_append(&sb, "'");
    } else {
        sb_append(&sb, "placeholder='Enter Creator Code'");
    }
    sb_appendf(&sb, " %s/><div id='battlepassCreatorCodeBtn' style='background-color:%s' onclick='creatorCodeAction(%d, 3)'>%s</div></div></div>\n\t\t\t</div>\n\t\t</div>",
               code[0] ? "disabled" : "", code[0] ? "#eb5656" : "#41c300", code[0] ? 0 : 1, code[0] ? "Remove" : "Save");
    sb_appendf(&sb, "\n\t\t<div class=\"bp-generic-btn-holder\">\n\t\t\t<div class='button bp-generic-btn' onmouseenter=\"playTick(0.1);\" onclick='playSelect(0.1);selectBPTier(this, 1);'>%s</div>\n\t\t</div>", price);
    char *html = sb_finish(&sb);
    generic_popup(html, "bp-purchase-pop");
    free(html);
}

static void skip_level_pop(void) {
    if (!g_bp) return;
    char price[32];
    price_text(price, sizeof price, g_bp->skip_price_kr);
    char html[2048];
    snprintf(html, sizeof html,
             "<div class=\"bp-generic-img-holder\">\n\t\t\t<div class=\"bp-generic-img-back\"></div>\n\t\t\t<div class=\"bp-generic-img bp-generic-img-skip\"></div>\n\t\t</div>\n\t\t<div class=\"bp-generic-text\">\n\t\t\t<div class=\"bp-generic-header\">Skip levels</div>\n\t\t\t<div class=\"bp-generic-info\">\n\t\t\t\tSkip Battle Pass level\n\t\t\t\t<div style=\"color: var(--yellow);font-size: 12px;\">*BPP limit will not increase*</div>\n\t\t\t</div>\n\t\t</div>\n\t\t<div class=\"bp-generic-btn-holder\">\n\t\t\t<div class='button bp-generic-btn-small' onmouseenter=\"playTick(0.1);\" onclick='playSelect(0.1);windows[5].increaseSkip(-1)'>-</div>\n\t\t\t<div id='bp-skip-num'>1</div>\n\t\t\t<div class='button bp-generic-btn-small' onmouseenter=\"playTick(0.1);\" onclick='playSelect(0.1);windows[5].increaseSkip(1)'>+</div>\n\t\t\t<div id=\"bp-skip-levels\" class='button bp-generic-btn' onmouseenter=\"playTick(0.1);\" onclick='playSelect(0.1);skipLevels(this)'>%s</div>\n\t\t</div>",
             price);
    generic_popup(html, "bp-generic-pop no-box-shadow");
}

static void skipped_level_pop(const int levels) {
    char html[1024];
    snprintf(html, sizeof html,
             "<div class=\"bp-generic-img-holder\">\n\t\t\t<div class=\"bp-generic-img-back\"></div>\n\t\t\t<div class=\"bp-generic-img bp-generic-img-skip\"></div>\n\t\t</div>\n\t\t<div class=\"bp-generic-text\">\n\t\t\t<div class=\"bp-generic-header\">%d levels skipped</div>\n\t\t\t<div class=\"bp-generic-info\">On the way to the top!</div>\n\t\t</div>\n\t\t<div class=\"bp-generic-btn-holder\">\n\t\t\t<div class='button bp-generic-btn' onmouseenter=\"playTick(0.1);\" onclick='playSelect(0.1);clearPops();'>Awesome!</div>\n\t\t</div>",
             levels);
    generic_popup(html, "bp-generic-pop");
}

static void purchased_pop(void) {
    generic_popup("<div class=\"bp-generic-img-holder\">\n\t\t\t<div class=\"bp-generic-img-back\"></div>\n\t\t\t<div class=\"bp-generic-img-splash\"></div>\n\t\t\t<div class=\"bp-generic-img\"></div>\n\t\t</div>\n\t\t<div class=\"bp-generic-text\">\n\t\t\t<div class=\"bp-generic-header\">VIP battle pass unlocked!</div>\n\t\t\t<div class=\"bp-generic-info\">You now have access to vip rewards!</div>\n\t\t</div>\n\t\t<div class=\"bp-generic-btn-holder\">\n\t\t\t<div class='button bp-generic-btn' onmouseenter=\"playTick(0.1);\" onclick='playSelect(0.1);clearPops();'>Awesome!</div>\n\t\t</div>",
                  "bp-generic-pop");
}

static int skip_count(void) {
    char text[16] = "";
    const HtmlElem el = html_dom_by_id("bp-skip-num");
    if (el) html_dom_get_text(el, text, sizeof text);
    const int n = atoi(text);
    return n > 0 ? n : 1;
}

// windows[5].increaseSkip(delta)
static void increase_skip(const int delta) {
    const HtmlElem el = html_dom_by_id("bp-skip-num");
    if (!el || !g_bp) return;
    int n = skip_count() + delta;
    const int max = bp_skip_max(g_bp, g_bpp); // limitMM(n, 1, max(1, itemcount - battlepassLvl))
    n = n < 1 ? 1 : n > max ? max : n;
    char text[32];
    snprintf(text, sizeof text, "%d", n);
    html_dom_set_text(el, text);
    const HtmlElem price = html_dom_by_id("bp-skip-levels");
    if (price) {
        price_text(text, sizeof text, (int64_t) g_bp->skip_price_kr * n);
        html_dom_set_text(price, text);
    }
}

// purchaseError(provider, error, product): #purchaseResponse with error.err
static void purchase_error(const cJSON *err) {
    const char *msg = cJSON_IsObject(err) ? jstr(err, "err") : cJSON_IsString(err) ? err->valuestring : "";
    char html[512];
    // + i18n "purchase.error.click" ("click here</a> and try again!") + "</a>"
    snprintf(html, sizeof html, "%s <a href='javascript:;' onclick='cancelPurchase()'>click here</a> and try again!</a>", msg && msg[0] ? msg : "Purchase Error");
    const HtmlElem h = html_dom_by_id("purchaseRespH");
    if (h) html_dom_set_html(h, html);
    const HtmlElem r = html_dom_by_id("purchaseResponse");
    if (r) html_dom_set_display(r, "block");
}

static void loading_popup(const char *text) { account_popup(2, NULL, text); } // io_loadingPopup

// ------------------------------------------------------------------ handlers

static int arg_num(const HtmlArg *args, const int argc, const int i, const int def) {
    if (i >= argc) return def;
    if (args[i].kind == HTML_ARG_NUM || args[i].kind == HTML_ARG_BOOL) return (int) args[i].num;
    if (args[i].kind == HTML_ARG_STR && args[i].str) return atoi(args[i].str);
    return def;
}

int battlepass_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    if (!name) return -1;
    if (!strcmp(name, "windows.5.switchTab")) {
        g_tab = arg_num(args, argc, 0, 0) ? 1 : 0;
        menu_rerender_if_open(BP_WINDOW); // updateWindow(6)
    } else if (!strcmp(name, "windows.5.updateScroll")) {
        update_scroll(arg_num(args, argc, 0, 1), arg_num(args, argc, 1, 0));
    } else if (!strcmp(name, "windows.5.updateScrollWheel")) {
        const double dy = ev && ev->value ? strtod(ev->value, NULL) : 0.0; // deltaY > 0 ? -1 : 1
        if (dy != 0.0) update_scroll(dy > 0.0 ? -1 : 1, 0);
    } else if (!strcmp(name, "windows.5.showBalloon") || !strcmp(name, "windows.5.hideBalloon")) {
        const HtmlElem b = html_dom_query(0, ".bpInfoBalloon");
        if (b) html_dom_set_display(b, !strcmp(name, "windows.5.showBalloon") ? "block" : "none");
    } else if (!strcmp(name, "windows.5.claimItem") || !strcmp(name, "windows.5.freeclaimItem")) {
        if (!account_get()) return 1;
        char frame[48];
        loading_popup("Claiming...");
        snprintf(frame, sizeof frame, "[\"bpclm\",%d]", arg_num(args, argc, 0, -1));
        backend_social_send_text(frame);
    } else if (!strcmp(name, "windows.5.purchasePop")) {
        if (account_get()) purchase_pop();
        else menu_show_window(5, 1);
    } else if (!strcmp(name, "windows.5.skipLevelPop")) {
        if (account_get()) skip_level_pop();
    } else if (!strcmp(name, "windows.5.increaseSkip")) {
        increase_skip(arg_num(args, argc, 0, 0));
    } else if (!strcmp(name, "selectBPTier") || !strcmp(name, "windows.5.selectBPTier")) {
        char frame[64];
        snprintf(frame, sizeof frame, "[\"xstbp\",%d,null]", arg_num(args, argc, 1, 1)); // io.send("xstbp", tier, creatorCode)
        if (account_get()) backend_social_send_text(frame);
    } else if (!strcmp(name, "skipLevels") || !strcmp(name, "windows.5.skipLevels")) {
        char frame[48];
        snprintf(frame, sizeof frame, "[\"xstbpl\",%d]", skip_count()); // io.send("xstbpl", levels)
        if (account_get() && html_dom_by_id("bp-skip-num")) backend_social_send_text(frame);
    } else if (!strcmp(name, "creatorCodeAction") && arg_num(args, argc, 1, 0) == 3) { // the purchase popup's creator code
        char code[128] = "";
        const HtmlElem in = html_dom_by_id("battlepassCreatorCode");
        if (in) html_dom_get_value(in, code, sizeof code);
        html_storage_set("krk_ccKN", arg_num(args, argc, 0, 0) ? code : "");
        purchase_pop();
    } else if (!strcmp(name, "cancelPurchase")) {
        const HtmlElem r = html_dom_by_id("purchaseResponse");
        if (r) html_dom_set_display(r, "none");
    } else {
        return -1;
    }
    return 1;
}

// ------------------------------------------------------------------ frames

int battlepass_ui_frame(const char *label, const cJSON *frame) {
    const cJSON *a1 = cJSON_GetArrayItem(frame, 1);
    if (!strcmp(label, "bpdl")) { // io_dataLoaded(data)
        set_data(cJSON_IsNull(a1) ? NULL : a1);
        g_bp_status = 2;
        menu_rerender_if_open(BP_WINDOW);
        if (g_after_purchase && g_bp) {
            g_after_purchase = 0;
            purchase_pop();
        }
    } else if (!strcmp(label, "bppd")) { // io_syncBpProg(BP)
        set_progress(a1);
        menu_rerender_if_open(BP_WINDOW);
    } else if (!strcmp(label, "bpclmr")) { // claimItemRes(_, _, skin, _, _, {id, tier, col, sk})
        const cJSON *sk = cJSON_GetArrayItem(frame, 3), *item = cJSON_GetArrayItem(frame, 6);
        if (account_get() && cJSON_IsNumber(sk) && sk->valueint > 0) account_add_skin(sk->valueint);
        if (cJSON_IsObject(item)) {
            char next[sizeof g_claimed];
            bp_add_claimed(g_claimed, (int) jnum(item, "id"), next, sizeof next);
            if (!bp_is_claimed(g_claimed, (int) jnum(item, "id"))) snprintf(g_claimed, sizeof g_claimed, "%s", next);
        }
        menu_rerender_if_open(BP_WINDOW);
        account_popup(1, NULL, "Item Claimed"); // io_showPopup("SUCCESS", null, "Item Claimed")
    } else if (!strcmp(label, "purbp")) { // purchaseRes(provider, error, product, tier, ...)
        const cJSON *err = cJSON_GetArrayItem(frame, 2);
        if (err && !cJSON_IsNull(err)) {
            purchase_error(err);
        } else {
            const cJSON *tier = cJSON_GetArrayItem(frame, 4);
            if (cJSON_IsNumber(tier)) g_tier = tier->valueint;
            menu_rerender_if_open(BP_WINDOW);
            purchased_pop();
        }
    } else if (!strcmp(label, "purbpl")) { // purchaseLvlRes(provider, error, _, levels, bpp, ...)
        const cJSON *err = cJSON_GetArrayItem(frame, 2), *levels = cJSON_GetArrayItem(frame, 4), *bpp = cJSON_GetArrayItem(frame, 5);
        if (err && !cJSON_IsNull(err)) {
            purchase_error(err);
        } else {
            if (cJSON_IsNumber(bpp)) g_bpp = (int64_t) bpp->valuedouble;
            menu_rerender_if_open(BP_WINDOW);
            skipped_level_pop(cJSON_IsNumber(levels) ? levels->valueint : 0);
        }
    } else if (!strcmp(label, "bpEnd")) { // the end screen's bpData (tmpBPData)
        cJSON_Delete(g_end);
        g_end = cJSON_IsObject(a1) ? cJSON_Duplicate(a1, 1) : NULL;
        hud_ui_end_refresh();
    } else {
        return 0;
    }
    return 1;
}

void battlepass_ui_challenges_changed(void) {
    if (g_tab == 1) menu_rerender_if_open(BP_WINDOW);
}

// ------------------------------------------------------------------ end screen

void battlepass_ui_end_clear(void) {
    cJSON_Delete(g_end);
    g_end = NULL;
}

// tabs.performance.gen(): #endBPLayout when (account && !isCustom && bpData)
int battlepass_ui_end_html(StrBuf *sb) {
    if (!g_end || !account_get()) return 0;
    const double mx = jnum(g_end, "mx") > 0 ? jnum(g_end, "mx") : 1, ml = jnum(g_end, "ml");
    const double bpp = jnum(g_end, "bpp");
    double points = fmod(bpp, mx);
    double pct = points / mx * 100.0;
    double level = floor(bpp / mx) + 1.0;
    const int complete = level >= ml;
    if (complete) {
        points = mx;
        pct = 100.0;
        level = ml;
    }
    const int vip = battlepass_ui_tier() > 0;
    const char *next_text = complete ? (vip ? "VIP Tier Complete" : "Free Tier Complete") : "Next Level Reward";
    const int locked = !complete && !vip;
    const cJSON *re = cJSON_GetObjectItemCaseSensitive(g_end, "re");
    char img[256] = "./img/battlepass/logo_white.svg";
    if (cJSON_IsObject(re)) {
        if (jnum(re, "kr")) snprintf(img, sizeof img, "./img/battlepass/krunkies.svg");
        else if (jnum(re, "sk") > 0) store_skin_preview((int) jnum(re, "sk"), img, sizeof img);
        else img[0] = 0;
    }
    if (complete) snprintf(img, sizeof img, "./img/battlepass/logo_0.png");
    sb_append(sb, "<div id='endBPLayout'>\n\t\t\t\t\t\t\t\t<div class=\"endSection\" style=\"margin-bottom:10px\">\n\t\t\t\t\t\t\t\t\t<div class='endTitle'>\n\t\t\t\t\t\t\t\t\t\tBattle Pass\n\t\t\t\t\t\t\t\t\t\t<img class=\"endBpInfo\"  onmouseenter=\"playTick()\" onclick=\"playSelect();showWindow(6)\" src=\"./img/frvr-ui/info-icon.svg\">\n\t\t\t\t\t\t\t\t\t</div>\n\t\t\t\t\t\t\t\t\t");
    sb_appendf(sb, "<div id='endBPLvl'>Level %.15g</div>\n\t\t\t\t\t\t\t\t\t<div class=\"blackShad\" id='endBPGained'>+%.15g</div>\n\t\t\t\t\t\t\t\t\t<div class=\"blackShad\" id='endBPProgText'>%.15g/%.15g</div>\n\t\t\t\t\t\t\t\t\t<div class='xpEndBar' style='margin-top: 19px;margin-bottom: 0px;'>\n\t\t\t\t\t\t\t\t\t\t<div class='xpEndBPBarB' style='width: %.15g%%;'></div>\n\t\t\t\t\t\t\t\t\t</div>\n\t\t\t\t\t\t\t\t\t",
               level, jnum(g_end, "gn") + jnum(g_end, "cbp"), points, jnum(g_end, "mx"), pct);
    sb_appendf(sb, "<div id=\"endBPUnlock\" onmouseenter=\"playTick(0.1);\" style=\"display:%s\" onclick=\"playSelect(0.1);windows[5].purchasePop();\">Unlock VIP</div>\n\t\t\t\t\t\t\t\t</div>\n\t\t\t\t\t\t\t\t",
               vip ? "none" : "block");
    sb_appendf(sb, "<div class=\"endBPNext\">\n\t\t\t\t\t\t\t\t\t<div id='endBPNextText'>%s</div>\n\t\t\t\t\t\t\t\t\t<img id=\"endBPLogo\" src=\"./img/battlepass/logo_white.svg\"%s>\n\t\t\t\t\t\t\t\t\t",
               next_text, complete ? " style=\"visibility:hidden\"" : "");
    sb_appendf(sb, "<div id=\"endBPReward\" style=\"display: %s;\">\n\t\t\t\t\t\t\t\t\t\t<img id=\"endBPRewardImg\" src=\"%s\">\n\t\t\t\t\t\t\t\t\t\t<img id=\"endBPRewardLock\" src=\"./img/battlepass/lock.png\" style=\"display:%s;\">\n\t\t\t\t\t\t\t\t\t</div>\n\t\t\t\t\t\t\t\t</div>\n\t\t\t\t\t\t\t</div>",
               cJSON_IsObject(re) || complete ? "block" : "none", img, locked ? "block" : "none");
    return 1;
}

// --render-scenario (ui_scenario.c): state.battlepass = {data, progress, tab, end} stands in for
// kn_brain's "bpdl" / "bppd" / "bpEnd".
void battlepass_ui_scenario(const cJSON *state) {
    battlepass_ui_reset();
    if (!cJSON_IsObject(state)) return;
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(state, "data");
    if (cJSON_IsObject(data)) {
        set_data(data);
        g_bp_status = 2;
    }
    set_progress(cJSON_GetObjectItemCaseSensitive(state, "progress"));
    g_tab = (int) jnum(state, "tab") ? 1 : 0;
    g_now_override = (int64_t) jnum(state, "now");
    const cJSON *end = cJSON_GetObjectItemCaseSensitive(state, "end");
    if (cJSON_IsObject(end)) g_end = cJSON_Duplicate(end, 1);
}
