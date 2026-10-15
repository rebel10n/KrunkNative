// Krunker v10.0.2's item tooltips (client/include/ui_tooltip.h), ported from window.createTip,
// createStreakTip, createSpinHistoryTip and cancelTip (g:216872-216907) and their helper
// (g:216638):
//
//   tooltip(el, html, gap = 5): every .tooltip removed; div.tooltip with html inserted before
//   document.body.firstChild; style "top:" + (el.top - (tip.height + gap) - 4) + "px;left:" +
//   el.left + "px;" from both getBoundingClientRect()s. main.css .tooltip styles it (and its
//   ::before arrow); cancelTip() removes every .tooltip.
//
// The item data: store.skins (the viewer table + assets/skins.json for lore), store.rarities,
// the market price estimates (social_estimate_value: v10's getPrices map; 0 = unknown), the kill
// streaks (g_streaks + v10 data/streaks.js descriptions) and the Spin History rows (store_ui.c).
// KrunkNative difference: player-made attribute values (name tags) are escaped; v10 inserts them raw.
#include <ui_tooltip.h>
#include <ui_logic.h>
#include <shared.h>
#include "../viewer/viewer_store.h"
#include <cJSON.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

const cJSON *ui_loadout_skin_json(int index);                         // ui_loadout.c: assets/skins.json's entry
int store_ui_spin_history_row(int i, int *ind, double *dte, int *fds); // store_ui.c: the loaded Spin History

#define TIP_GAP 5.0 // the helper's default third argument

// v10 data/streaks.js desc, by streak index (the order of g_streaks)
static const char *const STREAK_DESC[STREAK_COUNT] = {
    [STREAK_NUKE] = "Calls in a nuclear bomb that detonates after 10 seconds, killing all players that are spawned at the time the nuke goes off",
    [STREAK_AIRDROP] = "Calls in a air drop package containing various things.",
    [STREAK_REFILL] = "Dev Streak",
    [STREAK_BOMBARDMENT] = "Deploys a Bomber that carpet bombs the entire map",
    [STREAK_SLIMER] = "Equips the player with a slime gun that spawns slime pads which players gain a boost from",
    [STREAK_JUGGERNAUT] = "Equips the player with a Juggernaut Suit & Minigun",
    [STREAK_WAR_MACHINE] = "Equips the player with a Powerful Grenade Launcher",
};

void ui_tooltip_cancel(void) {
    HtmlElem tips[16];
    const int n = html_dom_query_all(0, ".tooltip", tips, 16);
    for (int i = 0; i < n; i++) html_dom_remove(tips[i]);
}

void ui_tooltip_show(const HtmlElem el, const char *html) {
    ui_tooltip_cancel();
    const HtmlElem body = html_dom_body();
    if (!el || !body) return;
    StrBuf sb = {0};
    sb_append(&sb, "<div class=\"tooltip\" id=\"knTip\">"); // id: KrunkNative (tests), no style
    sb_append(&sb, html ? html : "");
    sb_append(&sb, "</div>");
    char *markup = sb_finish(&sb);
    const HtmlElem first = html_dom_first_child(body);
    if (first) html_dom_insert_html(first, HTML_INSERT_BEFORE_BEGIN, markup);
    else html_dom_insert_html(body, HTML_INSERT_BEFORE_END, markup);
    free(markup);
    const HtmlElem tip = html_dom_by_id("knTip");
    HtmlRect at, box;
    if (!tip || !html_dom_get_rect(el, &at)) return;
    if (!html_dom_get_rect(tip, &box)) memset(&box, 0, sizeof(box));
    char style[96];
    snprintf(style, sizeof(style), "top:%.6gpx;left:%.6gpx;", (double) at.y - ((double) box.h + TIP_GAP) - 4.0, (double) at.x);
    html_dom_set_attr(tip, "style", style);
    printf("[tooltip] at %.0f,%.0f %.0fx%.0f\n", (double) at.x, (double) at.y - ((double) box.h + TIP_GAP) - 4.0, (double) box.w, (double) box.h);
}

static int attr_int(const HtmlElem el, const char *name, int *out) {
    char buf[32];
    if (html_dom_get_attr(el, name, buf, sizeof(buf)) < 0 || !buf[0]) return 0;
    char *end;
    const long v = strtol(buf, &end, 10); // parseInt
    if (end == buf) return 0;
    *out = (int) v;
    return 1;
}

static void format_num(char *out, const size_t n, const long long v) { // UTILS.formatNum: 1,234,567
    char d[32];
    snprintf(d, sizeof(d), "%lld", v < 0 ? -v : v);
    const size_t len = strlen(d);
    size_t o = 0;
    if (v < 0 && o + 1 < n) out[o++] = '-';
    for (size_t i = 0; i < len && o + 2 < n; i++) {
        if (i && (len - i) % 3 == 0) out[o++] = ',';
        out[o++] = d[i];
    }
    out[o] = 0;
}

static const char *rarity_color(const ViewerSkin *s) {
    const int r = s && s->rarity >= 0 && s->rarity < g_viewer_rarity_count ? s->rarity : 0;
    return g_viewer_rarity_count > 0 ? g_viewer_rarities[r].color : "#fff";
}

// "Est <span style='color: #FBC02D;'>" + (est == null ? "Unknown</span>" : formatNum(est) + "</span> KR")
static void est_html(StrBuf *sb, const int index, const char *label) {
    const int est = social_estimate_value(index);
    sb_appendf(sb, "%s<span style='color: #FBC02D;'>", label);
    if (est <= 0) {
        sb_append(sb, "Unknown</span>");
    } else {
        char num[32];
        format_num(num, sizeof(num), est);
        sb_appendf(sb, "%s</span> KR", num);
    }
}

void ui_tooltip_item(const HtmlElem el) {
    int index = -1;
    if (!el || !attr_int(el, "data-index", &index)) return;
    const ViewerSkin *s = viewer_skin(index);
    if (!s) return; // v10 reads skin.lore of undefined and throws
    char nametag[96] = "", wear[64] = "", wear_float[32] = "", serial[32] = "";
    const int has_tag = html_dom_get_attr(el, "data-nametag", nametag, sizeof(nametag)) >= 0 && nametag[0];
    const int has_wear = html_dom_get_attr(el, "data-wear", wear, sizeof(wear)) >= 0 && wear[0];
    const int has_float = html_dom_get_attr(el, "data-wear-float", wear_float, sizeof(wear_float)) >= 0 && wear_float[0];
    const int has_serial = html_dom_get_attr(el, "data-serial", serial, sizeof(serial)) >= 0 && serial[0];
    const char *lore = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(ui_loadout_skin_json(index), "lore"));
    const int is_free = (s->flags & VS_FREE) != 0;
    StrBuf sb = {0};
    if (has_tag) {
        sb_append(&sb, "<div class='tooltipSub' style='font-style:italic;font-size:10px;color:#ffdd57;'>&quot;");
        sb_append_esc(&sb, nametag);
        sb_append(&sb, "&quot;</div>");
    }
    if (lore && lore[0]) sb_appendf(&sb, "<div class='tooltipSub' style='font-style:italic;font-size:10px;'>%s</div>", lore);
    sb_appendf(&sb, "<div class='tooltipSub'><span style='color:%s'>", is_free ? "lightgrey" : rarity_color(s));
    sb_append_esc(&sb, s->name ? s->name : "");
    sb_append(&sb, "</span></div>");
    if (has_wear || has_serial) {
        sb_append(&sb, "<div class='tooltipSub' style='font-size:10px'>");
        if (has_wear) sb_append_esc(&sb, wear);
        if (has_float) {
            sb_append(&sb, " (");
            sb_append_esc(&sb, wear_float);
            sb_append(&sb, ")");
        }
        if (has_serial) {
            sb_append(&sb, "<span class='serialTag'>#");
            sb_append_esc(&sb, serial);
            sb_append(&sb, "</span>");
        }
        sb_append(&sb, "</div>");
    }
    if (!is_free) {
        sb_append(&sb, "<div class='tooltipSub' style='font-size:10px'>");
        est_html(&sb, index, "Est ");
        sb_append(&sb, "</div>");
    }
    char *html = sb_finish(&sb);
    ui_tooltip_show(el, html);
    free(html);
}

void ui_tooltip_streak(const HtmlElem el) {
    int i = -1;
    if (!el || !attr_int(el, "data-index", &i) || i < 0 || i >= STREAK_COUNT) return;
    char html[512];
    snprintf(html, sizeof(html),
             "<div class='tooltipSub'><span style='color:#FBC02D'>%s</span></div><div class='tooltipSub' style='font-style:italic;font-size:8px;max-width: 300px;'>%s</div>",
             g_streaks[i].name, STREAK_DESC[i] ? STREAK_DESC[i] : "");
    ui_tooltip_show(el, html);
}

// UTILS.getReadableTime(ms) with no flags: "[Ny ][Nd ][Nh ]Nm "
static void readable_time(char *out, const size_t n, const double ms) {
    long long s = (long long) floor((ms > 0.0 ? ms : 0.0) / 1000.0);
    long long m = s / 60;
    long long h = m / 60;
    m %= 60;
    long long d = h / 24;
    h %= 24;
    const long long y = d / 365;
    d %= 365;
    char yp[24] = "", dp[24] = "", hp[24] = "";
    if (y) snprintf(yp, sizeof(yp), "%lldy ", y);
    if (d) snprintf(dp, sizeof(dp), "%lldd ", d);
    if (h) snprintf(hp, sizeof(hp), "%lldh ", h);
    snprintf(out, n, "%s%s%s%lldm ", yp, dp, hp, m);
}

void ui_tooltip_spin_history(const HtmlElem el) {
    const HtmlElem parent = el ? html_dom_parent(el) : 0;
    if (!parent) return;
    int i = 0;
    for (HtmlElem c = html_dom_first_child(parent); c && c != el; c = html_dom_next(c)) {
        char tag[16] = "";
        html_dom_tag(c, tag, sizeof(tag));
        if (tag[0] >= 'a' && tag[0] <= 'z') i++; // parentElement.children: elements only
    }
    int ind = -1, fds = 0;
    double dte = 0.0;
    if (!store_ui_spin_history_row(i, &ind, &dte, &fds)) return;
    const ViewerSkin *s = viewer_skin(ind);
    if (!s) return;
    char ago[64], cost[32];
    readable_time(ago, sizeof(ago), (double) time(NULL) * 1000.0 - dte);
    format_num(cost, sizeof(cost), fds);
    StrBuf sb = {0};
    sb_appendf(&sb, "<div class='tooltipSub'><span style='color:%s'>", rarity_color(s));
    sb_append_esc(&sb, s->name ? s->name : "");
    sb_append(&sb, "</span></div><div class='tooltipSub' style='font-size:10px'>");
    est_html(&sb, ind, "Est: ");
    sb_appendf(&sb, "</div><div class='tooltipSub' style='font-size:10px'>Date: <span style='color: #FBC02D;'>%s ago</span></div>"
                    "<div class='tooltipSub' style='font-size:10px'>Cost: <span style='color: #FBC02D;'>%s KR</span></div>", ago, cost);
    char *html = sb_finish(&sb);
    ui_tooltip_show(el, html);
    free(html);
}

static HtmlElem this_arg(const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) args;
    (void) argc;
    return ev ? ev->target : 0; // createTip(this): the element the inline handler sits on
}

int ui_tooltip_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    if (!name) return -1;
    if (!strcmp(name, "createTip")) ui_tooltip_item(this_arg(ev, args, argc));
    else if (!strcmp(name, "createStreakTip")) ui_tooltip_streak(this_arg(ev, args, argc));
    else if (!strcmp(name, "createSpinHistoryTip")) ui_tooltip_spin_history(this_arg(ev, args, argc));
    else if (!strcmp(name, "cancelTip")) ui_tooltip_cancel();
    else return -1;
    return 1;
}
