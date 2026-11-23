// Rent Dedicated Server (Krunker v10.0.7 hostTypes[2], buyServ; client/include/rent_servers_ui.h,
// doc/v10/rented-servers.md). Krunker's markup and wording:
//   - windows[7].gen with buyServ (deob-10.0.7.js:219853-219938): tab 0 the regions with their free servers
//     ("N Available"), tab 1 the player's rented servers, tab 2 a region's servers (a rented one greyed, not
//     clickable), each a card with the game's thumbnail (user-assets /m<gid>/thumb.png); tab 3 one server: its
//     details, Time remaining, Rental Duration / Extend Time (#dediHostDur: 30 / 60 / 90 Days, CONF.typ
//     selected), Select / Change Game (showGames(2)), the terms and Server Cost (#dediCostI), then outside the
//     panel the message line (#dediHostErr), Rent Server / Extend Rental (#rentServBtn) and the owner's
//     Restart/Update (#updDediServBtn);
//   - switchTab2(i) (:219801-219805) clears the chosen game on tab 0; dediCst(v) the cost line (:219824-219826);
//   - rentDediServer(id, update) (:204642-204668): no game -> "Please select a game to host" in red; else both
//     buttons hidden, "Please Wait..." and ["rdsrv", id, {typ: #dediHostDur's value, mid, gnm}, update] (+ cr /
//     key: the published map's creator, the catalogue key, Box Office's additions that tell kn_rentd which map
//     to load);
//   - dediRes (:219811-219823): [0, rows] the list, redrawn; [1, text, keepHidden] the message (innerHTML,
//     white), the buttons back unless keepHidden.
// The list is asked for the first time the window is drawn (["dsrvl"], :219864-219866); after that only the
// brain's answers replace it.
#include <rent_servers_ui.h>

#include <backend.h>    // backend_social_send_text
#include <backend_ui.h> // active_account
#include <html_ui.h>
#include <i18n.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// iíïííïî: the rental regions {r, n, f, unlisted} (deob-10.0.7.js:18709-18777; REG indexes it)
static const struct {
    const char *region, *city, *flag;
    int unlisted;
} k_regions[] = {
    {"North America", "New York", "US", 0},     {"North America", "Sillicon Valley", "US", 0}, {"North America", "Dallas", "US", 0},
    {"North America", "Miami", "US", 1},        {"North America", "Seattle", "US", 0},         {"North America", "Chicago", "US", 0},
    {"North America", "Mexico", "MX", 0},       {"South America", "Sao Paolo", "BR", 0},       {"Middle East", "Manama", "BH", 0},
    {"East Asia", "Tokyo", "JP", 0},            {"Asia", "Singapore", "SG", 0},                {"East Asia", "Seoul", "KR", 0},
    {"South Asia", "Mumbai", "IN", 0},          {"Europe", "Frankfurt", "DE", 0},              {"Europe", "London", "GB", 0},
    {"Africa", "Cape Town", "ZA", 1},           {"Australia", "Sydney", "AU", 0},
};
#define REGION_COUNT ((int) (sizeof(k_regions) / sizeof(k_regions[0])))
// config.dediDurat / dediPrices (:18707-18708, :19647-19648)
static const int k_days[3] = {30, 60, 90};
static const int k_price[3] = {7500, 14000, 20000};

static struct {
    int tab;      // windows[7].tabIndex: 0 Rent Server, 1 My Servers, 2 a region, 3 a server
    int serv_ind; // windows[7].servInd: the region (tab 2) or the row (tab 3)
    cJSON *data;  // dediServers.data (owned); NULL until the list arrived
    int requested;
    int picking;  // showGames(2) is open for this window
    struct {
        char id[24], name[96], creator[48], key[96];
    } game;       // iîìîìíi: the chosen game ({id, name}); name "" = none
} R;

static int region_of(const cJSON *row) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(row, "REG");
    const int r = cJSON_IsNumber(v) ? v->valueint : -1;
    return r >= 0 && r < REGION_COUNT ? r : -1;
}
static double num(const cJSON *row, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(row, k);
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}
static const char *str(const cJSON *row, const char *k) {
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, k));
    return v ? v : "";
}

// iîìíïïí: thousands with commas
static void thousands(char *out, const size_t size, const long long v) {
    char raw[32];
    snprintf(raw, sizeof(raw), "%lld", v < 0 ? -v : v);
    const size_t len = strlen(raw);
    size_t o = 0;
    if (v < 0 && o + 1 < size) out[o++] = '-';
    for (size_t i = 0; i < len && o + 2 < size; i++) {
        if (i && (len - i) % 3 == 0) out[o++] = ',';
        out[o++] = raw[i];
    }
    out[o] = 0;
}

void rent_servers_ui_time(const double ms, char *out, const size_t size) {
    long long s = (long long) floor((ms > 0 ? ms : 0) / 1000.0);
    long long m = s / 60, h = m / 60, d = h / 24;
    m %= 60;
    h %= 24;
    const long long y = d / 365;
    d %= 365;
    char buf[96] = "";
    size_t o = 0;
    if (y) o += (size_t) snprintf(buf + o, sizeof(buf) - o, "%lldy ", y);
    if (d) o += (size_t) snprintf(buf + o, sizeof(buf) - o, "%lldd ", d);
    if (h) o += (size_t) snprintf(buf + o, sizeof(buf) - o, "%lldh ", h);
    snprintf(buf + o, sizeof(buf) - o, "%lldm ", m);
    snprintf(out, size, "%s", buf);
}

static const AccountInfo *me(void) { return active_account(); }
static int owns(const cJSON *row) {
    const AccountInfo *a = me();
    return a && a->id > 0 && num(row, "TRM") > 0 && (int) num(row, "OID") == a->id;
}

// JSON.parse(CONF || '{}') of a row with time left (a free row's is {}): NULL for {} (caller frees)
static cJSON *conf_of(const cJSON *row) {
    const char *t = str(row, "CONF");
    return num(row, "TRM") > 0 && t[0] ? cJSON_Parse(t) : NULL;
}

// iìïïiïî('/m' + gid + '/thumb.png'): the user-assets thumbnail, gid as JS prints it (undefined when the CONF has
// none). servers_ui.c's carousel reads the same KRUNKNATIVE_KRUNKER_USER_ASSETS override (tests, scenarios).
static void thumb_url(char *out, const size_t size, const cJSON *gid, const char *chosen) {
    const char *over = getenv("KRUNKNATIVE_KRUNKER_USER_ASSETS");
    const char *base = over && over[0] ? over : "https://user-assets.krunker.io";
    char g[64] = "undefined";
    if (chosen) snprintf(g, sizeof(g), "%s", chosen[0] ? chosen : "undefined");
    else if (cJSON_IsNumber(gid)) snprintf(g, sizeof(g), "%.15g", gid->valuedouble);
    else if (cJSON_IsString(gid)) snprintf(g, sizeof(g), "%s", gid->valuestring);
    else if (cJSON_IsNull(gid)) snprintf(g, sizeof(g), "null");
    else if (cJSON_IsBool(gid)) snprintf(g, sizeof(g), "%s", cJSON_IsTrue(gid) ? "true" : "false");
    snprintf(out, size, "%s/m%s/thumb.png", base, g);
}

// once per run, like dediServers; a draw before the social socket is up asks again on the next one
static void request_list(void) { R.requested = backend_social_send_text("[\"dsrvl\"]") != 0; }

static void rerender(void) { menu_rerender_if_open(MENU_WINDOW_HOST); }

// ------------------------------------------------------------------ markup

static void tab_bar(StrBuf *sb) {
    static const char *const names[2] = {"Rent Server", "My Servers"}; // buyTabs (:219806-219810)
    const int deep = R.tab >= 2;
    sb_append(sb, "<div class='bigMenTabs' style='margin-top:0px !important;margin-left:0px;'>");
    for (int i = 0; i < 2; i++) {
        sb_appendf(sb, "<div class='bigMenTab%s%s' onclick='playSelect();", R.tab == i ? " bigMenTabA" : "", deep ? " bigMenTabDis" : "");
        if (!deep) sb_appendf(sb, "windows[7].switchTab2(%d)", i);
        sb_append(sb, "' onmouseenter='playTick(0.1);'>");
        sb_append(sb, names[i]);
        sb_append(sb, "</div>");
    }
    if (deep)
        sb_append(sb, "<div class='bigMenTab bigMenTabA' onclick='playSelect();windows[7].switchTab2(0)' onmouseenter='playTick(0.1);'>"
                      "<span class='material-icons-outlined' style='font-size:40px;color:#fb5555;vertical-align:middle;margin-top:-12px;'>close</span></div>");
    sb_append(sb, "</div>");
}

// a serverCard: style (NULL = none), onclick (NULL = none), the thumbnail (NULL = none: tab 0's regions), the flag,
// the name and the side text
static void card(StrBuf *sb, const char *style, const char *onclick, const char *thumb, const char *flag, const char *title, const char *side) {
    sb_append(sb, "<div class='serverCard'");
    if (style) sb_appendf(sb, " style='%s'", style);
    if (onclick) sb_appendf(sb, " onclick='%s'", onclick);
    sb_append(sb, ">");
    if (thumb) {
        sb_append(sb, "<div style='background-image:url(");
        sb_append_esc(sb, thumb);
        sb_append(sb, ")' class='serverCardI'></div>");
    }
    sb_appendf(sb, "<img class='serverFlag' src='img/flags/3x2/%s.svg' />", flag);
    sb_append(sb, "<div class='serverCardInfo'><span style='padding-left:10px;color:#fff;'>");
    sb_append_esc(sb, title);
    sb_append(sb, "</span><span class='servCrdAv'>");
    sb_append_esc(sb, side);
    sb_append(sb, "</span></div></div>");
}

static void row_name(const cJSON *row, char *out, const size_t size) { snprintf(out, size, "%s_%d", str(row, "SVN"), (int) num(row, "SID")); }

static void tab_regions(StrBuf *sb) { // :219874-219880
    int free_in[REGION_COUNT] = {0};
    const cJSON *row;
    cJSON_ArrayForEach(row, R.data) {
        const int r = region_of(row);
        if (r >= 0 && num(row, "TRM") <= 0) free_in[r]++;
    }
    for (int i = 0; i < REGION_COUNT; i++) {
        if (k_regions[i].unlisted) continue;
        char click[96], side[32];
        snprintf(click, sizeof(click), "windows[7].servInd=%d;playSelect();windows[7].switchTab2(2);", i);
        snprintf(side, sizeof(side), "%d Available", free_in[i]);
        card(sb, NULL, click, NULL, k_regions[i].flag, k_regions[i].city, side);
    }
}

static void tab_mine(StrBuf *sb) { // :219881-219899
    if (!R.data) {
        sb_append(sb, "<div style='text-align:center;color:#fff'>Loading...</div>");
        return;
    }
    int shown = 0, i = 0;
    const cJSON *row;
    cJSON_ArrayForEach(row, R.data) {
        const int r = region_of(row);
        if (r >= 0 && owns(row)) {
            char click[96], name[96], thumb[256];
            snprintf(click, sizeof(click), "windows[7].servInd=%d;playSelect();windows[7].switchTab2(3);", i);
            row_name(row, name, sizeof(name));
            cJSON *conf = conf_of(row);
            thumb_url(thumb, sizeof(thumb), cJSON_GetObjectItemCaseSensitive(conf, "gid"), NULL);
            cJSON_Delete(conf);
            card(sb, NULL, click, thumb, k_regions[r].flag, name, "Cloud Compute");
            shown++;
        }
        i++;
    }
    if (!shown) sb_append(sb, "<div style='color:#fff;text-align:center;'>No available servers</div>");
}

static void tab_region(StrBuf *sb) { // :219900-219917
    if (!R.data) {
        sb_append(sb, "<div style='text-align:center;color:#fff;'>Loading...</div>");
        return;
    }
    int shown = 0, i = 0;
    const cJSON *row;
    cJSON_ArrayForEach(row, R.data) {
        if (region_of(row) == R.serv_ind) {
            const int rented = num(row, "TRM") > 0;
            char click[96], name[96], thumb[256];
            snprintf(click, sizeof(click), "windows[7].servInd=%d;playSelect();windows[7].switchTab2(3);", i);
            row_name(row, name, sizeof(name));
            cJSON *conf = conf_of(row);
            thumb_url(thumb, sizeof(thumb), cJSON_GetObjectItemCaseSensitive(conf, "gid"), NULL);
            cJSON_Delete(conf);
            card(sb, rented ? "filter:grayscale(100%);" : "", rented ? NULL : click, thumb, k_regions[R.serv_ind].flag, name, "Cloud Compute");
            shown++;
        }
        i++;
    }
    if (!shown) sb_append(sb, "<div style='color:#fff;text-align:center;'>No available servers</div>");
}

static void detail(StrBuf *sb, const char *label, const char *value) {
    sb_appendf(sb, "<div class='settName' style='font-size:20px;'>%s <span class='floatR'>", label);
    sb_append_esc(sb, value);
    sb_append(sb, "</span></div>");
}

// CONF.typ || 0 (a number, or the select's value as text)
static int conf_typ(const cJSON *conf) {
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(conf, "typ");
    const int v = cJSON_IsNumber(t) ? t->valueint : cJSON_IsString(t) ? atoi(t->valuestring) : 0;
    return v >= 0 && v < 3 ? v : 0;
}

static void tab_server(StrBuf *sb) { // :219918-219935
    const cJSON *row = cJSON_GetArrayItem(R.data, R.serv_ind);
    const int r = row ? region_of(row) : -1;
    if (r < 0) return;
    char name[96], text[160], thumb[256];
    row_name(row, name, sizeof(name));
    cJSON *conf = conf_of(row);
    const int mine = owns(row);
    const char *current = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(conf, "mn"));
    const int typ = conf_typ(conf);
    thumb_url(thumb, sizeof(thumb), cJSON_GetObjectItemCaseSensitive(conf, "gid"), R.game.name[0] ? R.game.id : NULL);
    card(sb, NULL, NULL, thumb, k_regions[r].flag, name, "Cloud Compute");
    sb_append(sb, "<div class='setBodH' style='width:731px;display:inline-block;margin-top:8px;margin-left:10px;'>");
    detail(sb, "Server Name", name);
    detail(sb, "Server Region", k_regions[r].region);
    detail(sb, "City", k_regions[r].city);
    detail(sb, "Type", "Cloud Compute single-core");
    sb_append(sb, "</div><div class='setBodH' style='margin-top:10px;'>");
    if (num(row, "TRM") > 0) {
        rent_servers_ui_time(num(row, "TRM"), text, sizeof(text));
        sb_append(sb, "<div class='settName' style='font-size:20px;padding-bottom:15px;padding-top:10px;'>Time remaining"
                      "<span style='float:right;color:#c2c2c2;'>");
        sb_append_esc(sb, text);
        sb_append(sb, "</span></div>");
    }
    sb_appendf(sb, "<div class='settName' style='font-size:20px;padding-bottom:15px;padding-top:10px;'>%s"
                   "<select onchange='windows[7].dediCst(this.value)' class='inputGrey2' style='margin-top:-5px;' id='dediHostDur'>",
               mine ? "Extend Time" : "Rental Duration");
    for (int i = 0; i < 3; i++) sb_appendf(sb, "<option value='%d' %s>%d Days</option>", i, i == typ ? " selected=''" : "", k_days[i]);
    sb_appendf(sb, "</select></div><div class='settName' style='font-size:20px;padding-bottom:15px;padding-top:10px;'>%s"
                   "<span class='floatR' style='margin-top:-5px;'><a href='javascript:showGames(2);'>",
               mine ? "Change Game" : "Select Game");
    sb_append_esc(sb, R.game.name[0] ? R.game.name : current && current[0] ? current : "Select Game");
    char cost[32];
    thousands(cost, sizeof(cost), k_price[typ]);
    sb_appendf(sb, "</a></span></div><div class='setBodH' style='margin-top:20px;padding-bottom:5px;padding-top:15px;'>"
                   "By renting a server, you agree to the <a>Terms &amp; Conditions</a>. It may take up to 10 minutes for a server to "
                   "become available once it starts up.<div class='settName' style='font-size:20px;padding-top:10px;border-top: 3px solid "
                   "rgba(255, 255, 255, 0.2);'>Server Cost<span class='floatR' style='color:#fff'><span id='dediCostI' "
                   "style='color:rgba(255,255,255,0.7)'>%s</span> KR</span></div></div></div>",
               cost);
    sb_append(sb, "<div style='font-size:22px;padding-left:10px;margin-top:20px;display:none;width:100%;text-align:center;' id='dediHostErr'></div>");
    const int id = (int) num(row, "ID");
    sb_appendf(sb, "<div id='rentServBtn' onclick='rentDediServer(`%d`);' style='%s'>%s</div>", id, mine ? "width:calc(50% - 20px);" : "",
               mine ? "Extend Rental" : "Rent Server");
    if (mine) sb_appendf(sb, "<div id='updDediServBtn' onclick='rentDediServer(%d, 1)'>Restart/Update</div>", id);
    cJSON_Delete(conf);
}

char *rent_servers_ui_gen(void) {
    if (!R.data && !R.requested) request_list();
    StrBuf sb = {0};
    tab_bar(&sb);
    if (R.tab == 0) tab_regions(&sb);
    else if (R.tab == 1) tab_mine(&sb);
    else if (R.tab == 2) tab_region(&sb);
    else tab_server(&sb);
    return sb_finish(&sb);
}

// The Host chooser's card (ui_windows.c gen_42). Its 14 px top margin makes the addition 107 css px, a whole 62 device
// px at the 1280x720 #uiBase scale, so the centred window moves up a whole 31 px and its five cards keep v10's
// sub-pixel phase (15 px moved it 31.3 px: text and images snapped a pixel apart; uidiff menu-host-type).
void rent_servers_ui_chooser_card(StrBuf *sb) {
    sb_append(sb, "<div class='serverHostOp serverHostOp-small' style='margin-top:14px;width:calc(100% - 12px);"
                  "background-image:url(&quot;img/host-bg/2.png&quot;);' onmouseenter='playTick()' "
                  "onclick='playSelect(0.1);openHostWindow(false, 2)'>");
    sb_append_esc(sb, I18N("kn.rent.card", "Rent a Server"));
    sb_append(sb, "</div>");
}

// ------------------------------------------------------------------ state changes

static cJSON *g_fixture; // a scenario's rows (rent_servers_ui_scenario): no brain to ask

// openHostWindow(false, 2) (:225502-225512): the chosen game cleared. dediServers is kept from the window's first
// draw (:219864-219866), so the list is not asked for again.
void rent_servers_ui_open(void) {
    R.tab = 0;
    R.picking = 0;
    memset(&R.game, 0, sizeof(R.game));
    if (g_fixture) {
        cJSON_Delete(R.data);
        R.data = cJSON_Duplicate(g_fixture, 1);
        R.requested = 1;
    }
}

void rent_servers_ui_scenario(const cJSON *fixture) {
    cJSON_Delete(g_fixture);
    const cJSON *rows = cJSON_GetObjectItemCaseSensitive(fixture, "rows");
    g_fixture = cJSON_IsArray(rows) ? cJSON_Duplicate(rows, 1) : NULL;
    if (!g_fixture) {
        cJSON_Delete(R.data);
        R.data = NULL;
        R.requested = 0;
    }
    rent_servers_ui_open();
    const char *game = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(fixture, "game"));
    if (game) snprintf(R.game.name, sizeof(R.game.name), "%s", game);
}

static void switch_tab2(const int tab) {
    if (tab == 0 && !g_fixture) memset(&R.game, 0, sizeof(R.game));
    R.tab = tab < 0 ? 0 : tab > 3 ? 3 : tab;
    rerender();
}

// #dediHostErr: shown, its innerHTML and colour; buttons 0 hides both, 1 shows both, -1 leaves them
static void show_message(const char *html, const char *color, const int buttons) {
    HtmlElem e = html_dom_by_id("dediHostErr");
    if (e) {
        html_dom_set_style(e, "display", "block");
        html_dom_set_html(e, html ? html : "");
        html_dom_set_style(e, "color", color);
    }
    if (buttons >= 0) {
        HtmlElem a = html_dom_by_id("rentServBtn"), b = html_dom_by_id("updDediServBtn");
        if (a) html_dom_set_style(a, "display", buttons ? "inline-block" : "none");
        if (b) html_dom_set_style(b, "display", buttons ? "inline-block" : "none");
    }
}

// rentDediServer(id, update): the game is the chosen one, else the shown server's (windows[7].servInd) CONF while
// it has time left; typ is #dediHostDur's value as text
static void rent(const char *id, const int update) {
    const cJSON *row = cJSON_GetArrayItem(R.data, R.serv_ind);
    cJSON *conf = row ? conf_of(row) : NULL;
    if (!R.game.name[0] && !conf) {
        show_message("Please select a game to host", "#ff3232", -1);
        return;
    }
    show_message("Please Wait...", "#fff", 0);
    char typ[16] = "0";
    HtmlElem dur = html_dom_by_id("dediHostDur");
    if (dur) html_dom_get_value(dur, typ, sizeof(typ));
    if (!typ[0]) snprintf(typ, sizeof(typ), "0");
    cJSON *frame = cJSON_CreateArray(), *game = cJSON_CreateObject();
    cJSON_AddItemToArray(frame, cJSON_CreateString("rdsrv"));
    char *end = NULL;
    const double n = strtod(id, &end); // `ID` (rent / extend) is text, ID (update) a number
    cJSON_AddItemToArray(frame, update && end && end != id && !*end ? cJSON_CreateNumber(n) : cJSON_CreateString(id));
    cJSON_AddStringToObject(game, "typ", typ);
    if (R.game.name[0]) {
        if (R.game.id[0]) cJSON_AddStringToObject(game, "mid", R.game.id); // undefined: left out, as JSON.stringify does
        cJSON_AddStringToObject(game, "gnm", R.game.name);
        if (R.game.creator[0]) cJSON_AddStringToObject(game, "cr", R.game.creator);
        if (R.game.key[0]) cJSON_AddStringToObject(game, "key", R.game.key);
    } else { // the server's own game (CONF gid / mn, and Box Office's cr / key)
        const cJSON *gid = cJSON_GetObjectItemCaseSensitive(conf, "gid"), *mn = cJSON_GetObjectItemCaseSensitive(conf, "mn");
        if (gid) cJSON_AddItemToObject(game, "mid", cJSON_Duplicate(gid, 1));
        if (mn) cJSON_AddItemToObject(game, "gnm", cJSON_Duplicate(mn, 1));
        const char *cr = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(conf, "cr"));
        const char *key = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(conf, "key"));
        if (cr && cr[0]) cJSON_AddStringToObject(game, "cr", cr);
        if (key && key[0]) cJSON_AddStringToObject(game, "key", key);
    }
    cJSON_AddItemToArray(frame, game);
    if (update) cJSON_AddItemToArray(frame, cJSON_CreateNumber(update));
    char *text = cJSON_PrintUnformatted(frame);
    cJSON_Delete(frame);
    cJSON_Delete(conf);
    // Box Office's: the social socket is down (Krunker's game socket queues the frame and the window waits)
    if (!text || !backend_social_send_text(text)) show_message(I18N("kn.rent.offline", "Not connected to Box Office servers."), "#ff3232", 1);
    free(text);
}

void rent_servers_ui_frame(const cJSON *frame) {
    const cJSON *kind = cJSON_GetArrayItem(frame, 1);
    if (!cJSON_IsNumber(kind)) return;
    if (kind->valueint == 0) { // the list, redrawn (the message line with it)
        const cJSON *rows = cJSON_GetArrayItem(frame, 2);
        cJSON_Delete(R.data);
        R.data = cJSON_IsArray(rows) ? cJSON_Duplicate(rows, 1) : cJSON_CreateArray();
        rerender();
    } else if (kind->valueint == 1) { // the request's answer
        const char *text = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 2));
        const cJSON *keep = cJSON_GetArrayItem(frame, 3);
        show_message(text ? text : "", "#fff", keep && !cJSON_IsFalse(keep) && !cJSON_IsNull(keep) && !(cJSON_IsNumber(keep) && keep->valuedouble == 0) ? -1 : 1);
    }
}

void rent_servers_ui_browse(void) {
    R.picking = 1;
    menu_show_window(10, 0); // the Games window; its Host button picks (selectHostMap -> rent_servers_ui_pick)
}

int rent_servers_ui_picking(void) { return R.picking; }

void rent_servers_ui_pick(const char *name, const char *creator, const char *key, const char *id) {
    R.picking = 0;
    snprintf(R.game.name, sizeof(R.game.name), "%s", name ? name : "");
    snprintf(R.game.creator, sizeof(R.game.creator), "%s", creator ? creator : "");
    snprintf(R.game.key, sizeof(R.game.key), "%s", key ? key : "");
    snprintf(R.game.id, sizeof(R.game.id), "%s", id ? id : "");
    menu_show_window(MENU_WINDOW_HOST, 1); // back to the server (tab 3 kept)
}

// ------------------------------------------------------------------ handlers

static int arg_num(const HtmlEvent *ev, const HtmlArg *args, const int argc, const int i, const int def) {
    if (i >= argc) return def;
    if (args[i].kind == HTML_ARG_NUM || args[i].kind == HTML_ARG_BOOL) return (int) args[i].num;
    if (args[i].kind == HTML_ARG_STR && args[i].str) return atoi(args[i].str);
    if (args[i].kind == HTML_ARG_THIS_VALUE && ev && ev->value) return atoi(ev->value);
    return def;
}

int rent_servers_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    if (!name) return -1;
    if (!strcmp(name, "windows.7.switchTab2")) {
        switch_tab2(arg_num(ev, args, argc, 0, 0));
        return 1;
    }
    if (!strcmp(name, "windows.7.servInd=")) {
        R.serv_ind = arg_num(ev, args, argc, 0, 0);
        return 1;
    }
    if (!strcmp(name, "windows.7.dediCst")) {
        int typ = arg_num(ev, args, argc, 0, 0);
        if (typ < 0 || typ > 2) typ = 0;
        char cost[32];
        thousands(cost, sizeof(cost), k_price[typ]);
        HtmlElem e = html_dom_by_id("dediCostI");
        if (e) html_dom_set_html(e, cost);
        return 1;
    }
    if (!strcmp(name, "rentDediServer")) {
        char id[32];
        if (argc > 0 && args[0].kind == HTML_ARG_STR && args[0].str) snprintf(id, sizeof(id), "%s", args[0].str);
        else snprintf(id, sizeof(id), "%d", arg_num(ev, args, argc, 0, 0));
        rent(id, arg_num(ev, args, argc, 1, 0));
        return 1;
    }
    return -1;
}
