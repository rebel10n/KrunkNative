// Krunker v3.1.1 menu Clans window (showWindow(13) = windows[12]) over the brain's social
// socket. Readable source: public/js/src/index.js windows[12] (gen, sendReq, resp, kick,
// updReq, updRol, chngFunds, calcFee, mangFunds, delReq / confDelClan / confKickClan) and
// src/config.js (clanTabs, clanRoles, maxClanContr, maxContrsT, clanWarLives).
//
// Frames (doc/backend.md 7.4): ["cln", action, value, extra] -> ["cln", msg, data, ok,
// action]: 1 create(tag) 2 request(tag) 3 my clan 4 leave/delete -> ["clnL"] 5 kick(id)
// 6 role(id, {r}) 7 funds(kr, {wd}); ["clnR", playerId, 1|0] answers a join request.
// Clan War contracts (8, startCont / showCont with the signature canvas) are not served by
// the brain ("Coming Soon"); the Clan War tab still lists the members by contract state.
#include <backend.h>
#include <backend_ui.h>
#include <html_ui.h>
#include <ui_logic.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLAN_RANK_VAR 0.03   // config.rankVar
#define CLAN_WAR_LIVES 250   // config.clanWarLives
#define CLAN_MAX_CONTR 30    // config.maxClanContr
#define CLAN_MAX_CONTRS_T 50 // config.maxContrsT
#define COL_PREMIUM "#FBC02D"         // COLORS.premium
#define COL_VERIFIED_PLAYER "#40C4FF" // COLORS.verified.player

static const char *const k_clan_tabs[] = {"Members", "Stats", "Requests", "Clan War"}; // config.clanTabs
static const struct {
    const char *name;
    int max;
    int perms;
} k_clan_roles[] = {{"Recruit", 0, 0}, {"Soldier", 25, 0}, {"Captain", 5, 1}, {"Commander", 1, 2}}; // config.clanRoles
#define CLAN_ROLES ((int) (sizeof(k_clan_roles) / sizeof(k_clan_roles[0])))

static cJSON *g_clan;   // windows[12].clanData (owned), NULL = not loaded
static int g_clan_tab;  // windows[12].tabIndex
static int g_req_sent;  // sendReq(3) in flight (the window asks once per load)

static double jnum(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItem(o, k);
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsString(v)) return atof(v->valuestring);
    if (cJSON_IsTrue(v)) return 1;
    return 0;
}

static const char *jstr(const cJSON *o, const char *k) {
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(o, k));
    return s ? s : "";
}

// UTILS.commaNumber
static void comma(char *out, const size_t n, const double value) {
    char digits[32];
    snprintf(digits, sizeof(digits), "%.0f", value);
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

static int my_id(void) {
    const AccountInfo *a = account_get();
    return a ? a->id : 0;
}

static int i_am_leader(void) {
    return g_clan && my_id() && (int) jnum(g_clan, "clan_playerid") == my_id();
}

static void send_cln(const int action, cJSON *value, cJSON *extra) {
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("cln"));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(action));
    cJSON_AddItemToArray(f, value ? value : cJSON_CreateNumber(1));
    if (extra) cJSON_AddItemToArray(f, extra);
    backend_social_send(f);
    cJSON_Delete(f);
}

void clan_ui_reset(void) {
    cJSON_Delete(g_clan);
    g_clan = NULL;
    g_clan_tab = 0;
    g_req_sent = 0;
}
// premium / verified icons + the profile link, shared by every member row
static void member_name(StrBuf *sb, const cJSON *m) {
    if (jnum(m, "su")) sb_append(sb, "<i class='material-icons' style='color:" COL_PREMIUM ";margin-top:-8px;font-size:28px;vertical-align:middle;margin-right:2px'>beenhere</i>");
    if (jnum(m, "ve")) sb_append(sb, "<i class='material-icons' style='color:" COL_VERIFIED_PLAYER ";margin-top:4px;font-size:28px;vertical-align:baseline;margin-right:2px'>check_circle</i>");
    sb_append(sb, "<a target='_blank'href='/social.html?p=profile&q=");
    sb_append_esc(sb, jstr(m, "p"));
    sb_append(sb, jnum(m, "h") ? "' style='color:#E85656'>" : "' >");
    sb_append_esc(sb, jstr(m, "p"));
    sb_append(sb, "</a>");
}

static int cmp_desc(const cJSON *a, const cJSON *b, const char *key) {
    const double x = jnum(a, key), y = jnum(b, key);
    return x < y ? 1 : x > y ? -1 : 0;
}

// members sorted by `key` descending (Array.sort b - a), borrowed pointers
static int sorted_members(const cJSON *members, const cJSON **out, const int max, const char *key) {
    int n = 0;
    const cJSON *m;
    cJSON_ArrayForEach(m, members) if (n < max) out[n++] = m;
    for (int i = 1; i < n; i++) { // stable insertion sort (JS sort is stable)
        const cJSON *v = out[i];
        int j = i - 1;
        while (j >= 0 && cmp_desc(out[j], v, key) > 0) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = v;
    }
    return n;
}

#define CLAN_MAX_MEMBERS 256

// tab 0: one section per role, Commander first (the creator is the Commander)
static void gen_members(StrBuf *sb, const cJSON **mem, const int n, const int leader_id, const int leader) {
    for (int r = CLAN_ROLES - 1; r >= 0; r--) {
        int count = 0;
        StrBuf rows = {0};
        for (int i = 0; i < n; i++) {
            const cJSON *m = mem[i];
            const int id = (int) jnum(m, "i");
            if (!((leader_id == id && k_clan_roles[r].perms == 2) || ((int) jnum(m, "r") == r && leader_id != id))) continue;
            count++;
            sb_append(&rows, "<div class='settName'>");
            member_name(&rows, m);
            if (leader && leader_id != id) {
                sb_appendf(&rows, "<div style='float:right;'><i style='font-size:40px;color:rgba(255,0,0,0.6)'  class='material-icons h' onclick='windows[12].kickReq(&quot;");
                sb_append_esc(&rows, jstr(m, "p"));
                sb_appendf(&rows, "&quot;,%d)'>clear</i><select class='clanRolD' onchange='windows[12].updRol(%d, this)' id='clanRolD%d'>", id, id, id);
                for (int o = CLAN_ROLES - 1; o >= 0; o--) {
                    if (k_clan_roles[o].perms == 2) continue;
                    sb_appendf(&rows, "<option value='%d' %s>%s</option>", o, (int) jnum(m, "r") == o ? "selected" : "", k_clan_roles[o].name);
                }
                sb_append(&rows, "</select></div>");
            }
            sb_append(&rows, "</div>");
        }
        char *body = sb_finish(&rows);
        sb_appendf(sb, "<div class='setHed' style='margin-top:20px'>%ss<span style='float:right;color:rgba(0,0,0,0.4)'>%d", k_clan_roles[r].name, count);
        if (k_clan_roles[r].max) sb_appendf(sb, "/%d", k_clan_roles[r].max);
        sb_append(sb, " <span style='color:#000'>Members</span></span></div>");
        if (body[0]) sb_append(sb, body);
        else sb_appendf(sb, "No %ss", k_clan_roles[r].name);
        free(body);
    }
}

// tab 1: Top Scorers (s7 = score of the last 7 days)
static void gen_stats(StrBuf *sb, const cJSON **mem, const int n) {
    sb_append(sb, "<div class='setHed'>Top Scorers <span style='float:right;color:rgba(0,0,0,0.4)'>Last 7 Days</span></div>");
    for (int i = 0; i < n; i++) {
        char v[32];
        comma(v, sizeof(v), jnum(mem[i], "s7"));
        sb_append(sb, "<div class='settName'>");
        member_name(sb, mem[i]);
        sb_appendf(sb, "<div style='float:right;color:rgba(0,0,0,0.8)'>%s</div></div>", v);
    }
}

// tab 2: Manage Requests
static void gen_requests(StrBuf *sb) {
    const cJSON *reqs = cJSON_GetObjectItem(g_clan, "requests");
    sb_appendf(sb, "<div class='setHed' style='margin-top:18px'>Manage Requests <span style='float:right;color:rgba(0,0,0,0.4)'>%d</span></div>", cJSON_GetArraySize(reqs));
    int i = 0;
    const cJSON *q;
    cJSON_ArrayForEach(q, reqs) {
        const int pid = (int) jnum(q, "cr_playerid");
        sb_append(sb, "<div class='settName'><a target='_blank' href='/social.html?p=profile&q=");
        sb_append_esc(sb, jstr(q, "player_name"));
        sb_append(sb, "'>");
        sb_append_esc(sb, jstr(q, "player_name"));
        sb_appendf(sb, "</a><span style='float:right'><i style='font-size:40px;color:rgba(255,0,0,0.6)'  class='material-icons h' onclick='windows[12].updReq(%d,%d, 0)'>clear</i><i style='font-size:40px;color:rgba(0,255,0,0.6);margin-left:10px' class='material-icons h' onclick='windows[12].updReq(%d,%d, 1)'>done</i></span></div>", i, pid, i, pid);
        i++;
    }
    if (!i) sb_append(sb, "<div style='margin-top:10px;color:rgba(0,0,0,0.4)'>No Requests</div>");
}
// tab 3: Clan War (contracts; st 1 active, 3 ended, de = deaths of the contract)
static void gen_war(StrBuf *sb, const cJSON **by_s7, const int n, const int leader) {
    const cJSON *mem[CLAN_MAX_MEMBERS];
    for (int i = 0; i < n; i++) mem[i] = by_s7[i];
    for (int i = 1; i < n; i++) { // sort by de ascending (stable)
        const cJSON *v = mem[i];
        int j = i - 1;
        while (j >= 0 && jnum(mem[j], "de") > jnum(v, "de")) {
            mem[j + 1] = mem[j];
            j--;
        }
        mem[j + 1] = v;
    }
    int counts[3] = {0, 0, 0};
    StrBuf parts[3] = {{0}, {0}, {0}};
    for (int i = 0; i < n; i++) {
        const cJSON *m = mem[i];
        const int st = (int) jnum(m, "st");
        const int dead = jnum(m, "de") >= CLAN_WAR_LIVES || st == 3;
        const int cat = dead ? 2 : st == 1 ? 0 : 1;
        const int lives = (int) fmax(0, CLAN_WAR_LIVES - jnum(m, "de"));
        int index = 0; // this.clanData.members.indexOf(hC): the s7-sorted array
        for (int k = 0; k < n; k++) if (by_s7[k] == m) index = k;
        counts[cat]++;
        StrBuf *p = &parts[cat];
        sb_append(p, "<div class='settName settNHov'>");
        member_name(p, m);
        if (dead) sb_appendf(p, "<span style='float:right;color:rgba(0,0,0,0.4);'>%d Kills</span>", (int) jnum(m, "ki"));
        if (!((!leader && st != 1) || dead)) {
            sb_append(p, "<div style='float:right;'>");
            if (cat == 0) sb_appendf(p, "<span style='color:rgba(0,0,0,0.4);vertical-align:top;margin-right:5px'>%d Lives</span>", lives);
            sb_appendf(p, "<i style='font-size:40px;color:#525252' title='Enlist'  class='material-icons h' onclick='windows[12].%s(%d)'>history_edu</i></div>", st == 1 ? "showCont" : "startCont", index);
        }
        sb_append(p, "</div>");
    }
    static const char *const none[3] = {"No Active Contracts", "No Reserves", "No KIA"};
    char *body[3];
    for (int i = 0; i < 3; i++) {
        body[i] = sb_finish(&parts[i]);
        if (!body[i][0]) {
            free(body[i]);
            body[i] = strdup(none[i]);
        }
    }
    sb_appendf(sb, "<div class='setHed' style='margin-top:16px'>Active Contracts<span style='float:right;color:rgba(0,0,0,0.6)'>%d/%d Active</span></div>%s", counts[0], CLAN_MAX_CONTR, body[0]);
    sb_appendf(sb, "<div class='setHed' style='margin-top:16px'>Killed in Action<span style='float:right;color:rgba(0,0,0,0.6)'>%d Players</span></div>%s", counts[2], body[2]);
    sb_appendf(sb, "<div class='setHed' style='margin-top:16px'>Reserves<span style='float:right;color:rgba(0,0,0,0.6)'>%d RSV</span></div>%s", counts[1], body[1]);
    for (int i = 0; i < 3; i++) free(body[i]);
}

static void send_req(const int action, const char *value);

// the clan page of the signed-in member (clanData loaded)
static void gen_clan_page(StrBuf *sb) {
    const cJSON *members = cJSON_GetObjectItem(g_clan, "members");
    const cJSON *mem[CLAN_MAX_MEMBERS];
    const int leader_id = (int) jnum(g_clan, "clan_playerid");
    const int leader = i_am_leader();
    // level of clan_score (the same curve as accounts)
    const double score = jnum(g_clan, "clan_score");
    const double lv = CLAN_RANK_VAR * sqrt(score);
    int level = (int) floor(lv);
    const int prog = (int) round((lv - level) * 100);
    if (level < 1) level = 1;
    const double base = level > 1 ? pow(level / CLAN_RANK_VAR, 2) : 0;
    char funds[32], total[32], cur[32], need[32];
    comma(funds, sizeof(funds), jnum(g_clan, "clan_funds"));
    comma(total, sizeof(total), score);
    comma(cur, sizeof(cur), (double) (long long) (score - base));
    comma(need, sizeof(need), (double) (long long) (pow((level + 1) / CLAN_RANK_VAR, 2) - base));
    sb_appendf(sb, "<div class='settName'>Rank<span class='floatR'>0</span></div><div class='settName'>Bank<span class='floatR'><span style='color:rgba(0,0,0,0.4)'><a href='javascript:;'  onclick='windows[12].mangFunds()' class='menuLink'>%s</a></span> KR</span></div>", funds);
    sb_appendf(sb, "<div class='settName'>Level<span class='floatR'>%d</span></div><div class='settName'>Total Score<span class='floatR'>%s</span></div>", level, total);
    sb_appendf(sb, "<div class='xpBar' style='margin-bottom:10px'><div class='xpBarB' style='width:%d%%'></div><span class='xpBarV'>%s / %s</span></div>", prog, cur, need);
    sb_append(sb, "<div class='menuTabsNew' style='margin-top:15px;'>");
    for (int i = 0; i < 4; i++) sb_appendf(sb, "<div class='menuTabNew%s' onclick='windows[12].switchTab(%d)'>%s</div>", g_clan_tab == i ? " tabANew" : "", i, k_clan_tabs[i]);
    sb_append(sb, "</div>");
    if (g_clan_tab == 1) {
        const int n = sorted_members(members, mem, CLAN_MAX_MEMBERS, "s7");
        gen_stats(sb, mem, n);
    } else if (g_clan_tab == 3) {
        sb_appendf(sb, "<a class='menuLink' target='blank_' href='/social.html?p=clanwars' style='margin-top:10px'>View World Map</a><span style='float:right;color:rgba(0,0,0,0.5)'>%d/%d <span style='color:rgba(0,0,0,0.8)'>Contracts used</span></span>",
                   (int) jnum(g_clan, "clan_contracts"), CLAN_MAX_CONTRS_T);
        const int n = sorted_members(members, mem, CLAN_MAX_MEMBERS, "s7");
        gen_war(sb, mem, n, leader);
    } else if (g_clan_tab == 0) {
        const int n = sorted_members(members, mem, CLAN_MAX_MEMBERS, "s");
        gen_members(sb, mem, n, leader_id, leader);
    } else {
        gen_requests(sb);
    }
}

// windows[12].gen()
char *clan_window_gen(void) {
    StrBuf sb = {0};
    const AccountInfo *a = account_get();
    if (!a) {
        sb_append(&sb, "<div class='reqW login' onclick='showWindow(5)'><i class='material-icons reqWIcon'>clear</i><div class='reqWLvl'>LOGIN REQUIRED</div><div class='reqWTxt'>to create and join a clan!</div></div>");
        return sb_finish(&sb);
    }
    if (a->clan[0]) {
        sb_append(&sb, "<div class='setHed' style='margin-top:0px'>[");
        sb_append_esc(&sb, a->clan);
        sb_appendf(&sb, "] Clan Page<a style='float:right' href='javascript:;' onclick='windows[12].delReq()' class='menuLink'>%s</a></div><div id='clanErr' style='margin-top:0px;color:rgba(0,0,0,0.5)'>",
                   g_clan ? (i_am_leader() ? "Delete" : "Leave") : "");
        if (g_clan) gen_clan_page(&sb);
        else sb_append(&sb, "LOADING...");
        sb_append(&sb, "</div>");
        if (!g_clan && !g_req_sent) {
            g_req_sent = 1;
            send_cln(3, NULL, NULL); // this.sendReq(3)
        }
        return sb_finish(&sb);
    }
    // windows[12].gen() without a clan (v7.1.4)
    sb_append(&sb, "<div class='setHed' style='margin-top:-5px'>Create Clan</div><input id='clanInp1' type='text' class='smlInput' maxlength='4' placeholder='Clan Name' /><a class='clanSubmitBtn' style='margin-left:20px' onclick='windows[12].sendReq(1)'>Create</a><div class='setHed'>Join Clan <a href='./social.html?p=leaders&q=clans' target='_blank' class='menuLink' style='float:right'>View Clans</a></div><input id='clanInp2' type='text' class='smlInput' maxlength='5' placeholder='Clan Name' /><a class='clanSubmitBtn' style='margin-left:20px' onclick='windows[12].sendReq(2)'>Submit</a><div id='clanErr' style='margin-top:10px'></div>");
    return sb_finish(&sb);
}
// ---- popups (#popupHolder: confPop / popupContent, index.js resetPops / dP / contPopup)

static void set_disp(const char *id, const char *v) {
    HtmlElem e = html_dom_by_id(id);
    if (e) html_dom_set_display(e, v);
}

static void reset_pops(void) {
    static const char *const pops[] = {"popupContent", "confPremiumPop", "confPop", "gameCodePop", "tradePop", "twitchPop", "pfpPop", "dropsPop"};
    set_disp("popupHolder", "block");
    for (size_t i = 0; i < sizeof(pops) / sizeof(pops[0]); i++) set_disp(pops[i], "none");
    HtmlElem c = html_dom_by_id("popupContent");
    if (c) html_dom_set_style(c, "background-color", "rgba(0, 0, 0, 0.75)");
}

static void content_popup(const char *width, const char *html) {
    reset_pops();
    HtmlElem c = html_dom_by_id("popupContent");
    if (!c) return;
    html_dom_set_style(c, "width", width);
    html_dom_set_display(c, "block");
    html_dom_set_html(c, html);
}

static void conf_popup(const char *html) {
    reset_pops();
    HtmlElem c = html_dom_by_id("confPop");
    if (!c) return;
    html_dom_set_display(c, "block");
    html_dom_set_html(c, html);
}

static void wait_popup(void) { // dP("Please Wait...")
    content_popup("300px", "<div style='width:100%;text-align:center'><div class='lds-ring sm'><div></div><div></div><div></div><div></div></div><div style='font-size:20px;color:rgba(255,255,255,0.5)'>Please Wait...</div></div>");
}

// io_pRes(kind, error, text)
static void result_popup(const int ok, const char *text) {
    char html[512];
    snprintf(html, sizeof(html), "<div style='width:100%%;text-align:center'><div><i class='material-icons' style='font-size:80px;color:%s'>%s</i></div><div style='font-size:20px;color:rgba(255,255,255,0.5)'>%s</div></div>",
             ok ? "#3be354" : "#e33b3b", ok ? "done" : "clear", text && text[0] ? text : ok ? "Success" : "Error");
    content_popup("300px", html);
}

static void clan_err(const char *msg, const int ok) {
    HtmlElem e = html_dom_by_id("clanErr");
    if (!e) return;
    html_dom_set_style(e, "color", ok ? "rgba(0,0,0,0.6)" : "rgba(255,0,0,0.6)");
    html_dom_set_html(e, msg);
}

static void send_req(const int action, const char *value) {
    HtmlElem e = html_dom_by_id("clanErr");
    if (e) {
        html_dom_set_style(e, "color", "rgba(0,0,0,0.6)");
        html_dom_set_html(e, "LOADING...");
    }
    send_cln(action, value && value[0] ? cJSON_CreateString(value) : cJSON_CreateNumber(1), NULL);
}

static cJSON *member_by_id(const int id) {
    cJSON *m;
    cJSON_ArrayForEach(m, cJSON_GetObjectItem(g_clan, "members")) if ((int) jnum(m, "i") == id) return m;
    return NULL;
}

static int arg_num(const HtmlArg *args, const int argc, const int i, const int def) {
    if (i >= argc) return def;
    if (args[i].kind == HTML_ARG_NUM) return (int) args[i].num;
    if (args[i].kind == HTML_ARG_STR && args[i].str) return atoi(args[i].str);
    return def;
}

// mangFunds(): the Manage Clan Funds popup
static void manage_funds(void) {
    char funds[32], html[1536];
    comma(funds, sizeof(funds), jnum(g_clan, "clan_funds"));
    const int leader = i_am_leader();
    snprintf(html, sizeof(html),
             "<div style='font-size:21px;color:#fff'>Manage Clan Funds<span style='float:right;color:rgba(255,255,255,0.6)'>%s <span style='color:#fff'>KR</span></span></div><div style='margin-top:20px;font-size:18px;color:#FFF'>Deposit Fee<span id='clnFeeVal' style='float:right;color:rgba(255,255,255,0.7)'>None</span></div><input type='number' oninput='windows[12].calcFee(this)' placeholder='Enter KR Amount' id='clanFundsIn'></input><div class='clanFndB' style='background-color:%s' %s>Withdraw</div><div class='clanFndB' style='margin-left:16px;background-color:#4ca9f5' onclick='windows[12].chngFunds(0)'>Deposit</div>",
             funds, leader ? "#fc5a4e" : "#6e6e6e", leader ? "onclick='windows[12].chngFunds(1)'" : "");
    content_popup("700px", html);
}

// calcFee(input): 10% deposit fee preview
static void calc_fee(void) {
    HtmlElem in = html_dom_by_id("clanFundsIn");
    char v[32] = "";
    if (in) html_dom_get_value(in, v, sizeof(v));
    long long kr = atoll(v);
    if (kr < 0) kr = 0;
    const long long fee = kr / 10;
    HtmlElem out = html_dom_by_id("clnFeeVal");
    if (!out) return;
    if (!fee) {
        html_dom_set_html(out, "None");
        return;
    }
    char f[32], html[192];
    comma(f, sizeof(f), (double) fee);
    snprintf(html, sizeof(html), "<span style='color:rgba(255,255,255,0.7)'>-%s <span style='color:#fff'>KR</span></span>", f);
    html_dom_set_html(out, html);
}
int clan_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    if (!name || strncmp(name, "windows.12.", 11) && strcmp(name, "delClanConfirmed") && strcmp(name, "kickClanConfirmed")) return -1;
    const char *m = name + 11;
    if (!strcmp(name, "delClanConfirmed")) { // dP + io.send("cln", 4, 1)
        wait_popup();
        send_cln(4, NULL, NULL);
    } else if (!strcmp(name, "kickClanConfirmed")) { // clearPops(); windows[12].kick(id)
        ui_loadout_clear_pops();
        const int id = arg_num(args, argc, 0, 0);
        send_cln(5, cJSON_CreateNumber(id), NULL);
        cJSON *members = cJSON_GetObjectItem(g_clan, "members");
        for (int i = cJSON_GetArraySize(members) - 1; i >= 0; i--) {
            if ((int) jnum(cJSON_GetArrayItem(members, i), "i") == id) cJSON_DeleteItemFromArray(members, i);
        }
        menu_rerender_if_open(13);
    } else if (!strcmp(m, "sendReq")) {
        const int action = arg_num(args, argc, 0, 1);
        char value[32] = "", id[16];
        snprintf(id, sizeof(id), "clanInp%d", action);
        HtmlElem in = html_dom_by_id(id);
        if (in) html_dom_get_value(in, value, sizeof(value));
        send_req(action, value);
    } else if (!strcmp(m, "switchTab")) {
        g_clan_tab = arg_num(args, argc, 0, 0);
        menu_rerender_if_open(13);
    } else if (!strcmp(m, "kickReq")) { // confKickClan(name, id)
        const char *who = argc > 0 && args[0].kind == HTML_ARG_STR && args[0].str ? args[0].str : "";
        StrBuf sb = {0};
        sb_append(&sb, "<div style='color:#fff;font-size:20px;position:relative;margin-bottom:15px;'>Are you sure you want kick ");
        sb_append_esc(&sb, who);
        sb_appendf(&sb, " from the clan?</div><div id='confirmBtn' onclick='kickClanConfirmed(%d)' onmouseover='SOUND.play(&quot;tick_0&quot;,0.1)'>YES</div><div id='declineBtn' onclick='clearPops()' onmouseover='SOUND.play(&quot;tick_0&quot;,0.1)'>NO</div>", arg_num(args, argc, 1, 0));
        char *html = sb_finish(&sb);
        conf_popup(html);
        free(html);
    } else if (!strcmp(m, "delReq") && g_clan) { // confDelClan(isLeader)
        char html[640];
        snprintf(html, sizeof(html), "<div style='color:#fff;font-size:20px;position:relative;margin-bottom:15px;'>Are you sure you want to %s the clan?</div><div id='confirmBtn' onclick='delClanConfirmed()' onmouseover='SOUND.play(&quot;tick_0&quot;,0.1)'>YES</div><div id='declineBtn' onclick='clearPops()' onmouseover='SOUND.play(&quot;tick_0&quot;,0.1)'>NO</div>",
                 i_am_leader() ? "delete" : "leave");
        conf_popup(html);
    } else if (!strcmp(m, "updReq") && g_clan) { // io.send("clnR", id, ok); local list update
        const int index = arg_num(args, argc, 0, 0), pid = arg_num(args, argc, 1, 0), ok = arg_num(args, argc, 2, 0);
        char frame[64];
        snprintf(frame, sizeof(frame), "[\"clnR\",%d,%d]", pid, ok);
        backend_social_send_text(frame);
        cJSON *reqs = cJSON_GetObjectItem(g_clan, "requests");
        cJSON *members = cJSON_GetObjectItem(g_clan, "members");
        const cJSON *req = cJSON_GetArrayItem(reqs, index);
        if (req && ok == 1 && members) {
            cJSON *nm = cJSON_CreateObject();
            cJSON_AddStringToObject(nm, "p", jstr(req, "player_name"));
            cJSON_AddItemToArray(members, nm);
        }
        if (req) cJSON_DeleteItemFromArray(reqs, index);
        menu_rerender_if_open(13);
    } else if (!strcmp(m, "updRol")) { // dP + io.send("cln", 6, id, {r})
        const int id = arg_num(args, argc, 0, 0);
        char sel[24], v[8] = "";
        snprintf(sel, sizeof(sel), "clanRolD%d", id);
        HtmlElem s = html_dom_by_id(sel);
        if (s) html_dom_get_value(s, v, sizeof(v));
        wait_popup();
        cJSON *extra = cJSON_CreateObject();
        cJSON_AddNumberToObject(extra, "r", atoi(v));
        send_cln(6, cJSON_CreateNumber(id), extra);
    } else if (!strcmp(m, "mangFunds") && g_clan) {
        manage_funds();
    } else if (!strcmp(m, "calcFee")) {
        calc_fee();
    } else if (!strcmp(m, "chngFunds")) { // dP + io.send("cln", 7, kr, {wd})
        HtmlElem in = html_dom_by_id("clanFundsIn");
        char v[32] = "";
        if (in) html_dom_get_value(in, v, sizeof(v));
        const long long kr = atoll(v);
        if (kr) {
            wait_popup();
            cJSON *extra = cJSON_CreateObject();
            cJSON_AddNumberToObject(extra, "wd", arg_num(args, argc, 0, 0));
            send_cln(7, cJSON_CreateNumber((double) kr), extra);
        }
    } else if (!strcmp(m, "startCont") || !strcmp(m, "showCont")) {
        result_popup(0, "Coming Soon"); // Clan War contracts: not served by the brain (cln 8)
    } else {
        return -1;
    }
    return 1;
}

// windows[12].resp(msg, data, ok, action) / io_clnL()
void clan_ui_frame(const cJSON *frame) {
    const char *label = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 0));
    if (!label) return;
    if (!strcmp(label, "clnL")) { // del(): clanData = null; acc.clan = null; showWindow(5)
        clan_ui_reset();
        account_set_clan(NULL);
        ui_loadout_clear_pops();
        menu_show_window(5, 1);
        return;
    }
    if (strcmp(label, "cln") || cJSON_GetArraySize(frame) < 5) return; // ["cln", msg, ok]: the hub page's join reply
    const char *msg = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 1));
    const cJSON *data = cJSON_GetArrayItem(frame, 2);
    const int ok = (int) cJSON_GetNumberValue(cJSON_GetArrayItem(frame, 3)) || cJSON_IsTrue(cJSON_GetArrayItem(frame, 3));
    const int action = (int) cJSON_GetNumberValue(cJSON_GetArrayItem(frame, 4));
    const int has_data = cJSON_IsObject(data);
    if (action == 7) {
        if (ok && has_data) {
            account_set_funds(jnum(data, "pf"));
            if (g_clan) cJSON_ReplaceItemInObject(g_clan, "clan_funds", cJSON_CreateNumber(jnum(data, "cf")));
            menu_rerender_if_open(13);
            result_popup(1, "Success");
        } else {
            result_popup(0, msg ? msg : "Error");
        }
        return;
    }
    if (action == 6) {
        if (ok && has_data) {
            cJSON *mm = member_by_id((int) jnum(data, "i"));
            if (mm) {
                cJSON_DeleteItemFromObject(mm, "r");
                cJSON_AddNumberToObject(mm, "r", jnum(data, "r"));
                menu_rerender_if_open(13);
            }
            result_popup(1, "Success");
        } else {
            result_popup(0, msg ? msg : "Error");
        }
        return;
    }
    if (action == 3) {
        g_req_sent = 0;
        if (!has_data && !(msg && msg[0])) { // not in a clan any more (kicked, deleted): the create/join form
            account_set_clan(NULL);
            menu_rerender_if_open(13);
            return;
        }
    }
    if (msg && msg[0]) {
        clan_err(msg, ok);
    } else if (has_data && cJSON_GetObjectItem(data, "clan_name")) {
        account_set_clan(jstr(data, "clan_name"));
        cJSON_Delete(g_clan);
        g_clan = cJSON_Duplicate(data, 1);
        menu_rerender_if_open(13);
    }
}
