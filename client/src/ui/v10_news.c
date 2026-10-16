// Krunker v10.0.2's What's New, Guide and Community & Events (doc/v10/ui.md "What's New, Guide, Community"):
//   - windows[34] Changelog / "What's New" (showWindow(35); the menu's #updateAd item, the footer's
//     Changelog link): v10's gen / toggleLog / switchTab / viewAll over Krunker Box Office's own
//     changelog, <page>/windows/kn-versions.txt (Krunker's docs/versions.txt format; the page's
//     versions.txt, Krunker's own, when a page tree has none). Its newest update is the version
//     (v10's config.gameVersion). KN addition: the brain's news items (GET /social/news, v10's hub
//     News tab feed) as v10 .news-item cards above the notes, when there are any.
//   - openGuide(section): v10's tutorialPopup (_66971, svelte-los391) with its twelve sections,
//     the img/guide/* images (tools/ui-import/live_assets_v10.lock) and the Basics video
//     (videos/tutorial/basics.mp4; natively the engine plays its .knv frame sidecar, engine/video.h,
//     with its first frame, ui/img/guide/kn_basics_poster.jpg, as the poster while that is missing).
//   - openCommunityMenu(): v10's communityPopup (_66842, svelte-1ley7u4): polls and events from
//     the brain's GET /creators/events (services/brain/community.c), the socials footer.
// The v10 menu's Svelte items (#menuBtnGuide, #updateAd, #menuBtnSideCommunity) get their v10
// actions as inline handlers once the page is up (v10_news_setup).
#include <backend.h>
#include <backend_ui.h>
#include <ui_logic.h>
#include <v10_misc_popups.h>
#include <html_ui.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NEWS_TTL_MS 60000
#define EVENTS_TTL_MS 60000

static void reset_generic_popup(const char *cls) {
    ui_loadout_reset_pops(); // resetPops: #popupHolder up, every other popup hidden
    HtmlElem pop = html_dom_by_id("genericPop");
    if (!pop) return;
    html_dom_set_attr(pop, "class", cls);
    html_dom_set_attr(pop, "style", "");
}

// genericPopup({component}, cls): the component's markup in #genericPop
static void generic_popup(const char *cls, const char *html) {
    reset_generic_popup(cls);
    HtmlElem pop = html_dom_by_id("genericPop");
    if (!pop) return;
    html_dom_set_html(pop, html);
    html_dom_set_display(pop, "block");
}

// #genericPop shows this popup (its class)
static int popup_open(const char *cls) {
    HtmlElem pop = html_dom_by_id("genericPop"), holder = html_dom_by_id("popupHolder");
    char c[64] = "", d[16] = "";
    if (!pop || html_dom_get_attr(pop, "class", c, sizeof(c)) < 0 || strcmp(c, cls)) return 0;
    if (holder && html_dom_get_style(holder, "display", d, sizeof(d)) >= 0 && !strcmp(d, "none")) return 0;
    return 1;
}

static void sb_img(StrBuf *sb, const char *path) {
    char url[256];
    ui_img_url(path, url, sizeof(url));
    sb_append_esc(sb, url);
}

static const char *jstr(const cJSON *o, const char *k) {
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(o, k));
    return s ? s : "";
}

// "2026-10-07T11:11:00.000Z" (UTC) -> unix s; -1
static long long parse_iso(const char *s) {
    int y, mo, d, h = 0, mi = 0, sec = 0;
    if (!s || sscanf(s, "%4d-%2d-%2d", &y, &mo, &d) != 3) return -1;
    if (strlen(s) > 10 && (s[10] == 'T' || s[10] == ' ') && sscanf(s + 11, "%2d:%2d:%2d", &h, &mi, &sec) < 2) return -1;
    // days from civil (Howard Hinnant), no timegm on every platform
    y -= mo <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const long long yoe = y - era * 400;
    const long long doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (era * 146097 + doe - 719468) * 86400 + h * 3600 + mi * 60 + sec;
}

// ================================================================ windows[34] Changelog / What's New

// v10 splits docs/versions.txt with
//   /^\s*== (?=.*?\bUPDATE|UPADTE|INITIAL|TRICKSHOT\b)(.*) ==/gm
// into [prefix, title, body, title, body, ...]; each pair is an update whose version is the
// title's first /[\d\.]+/, grouped by its first number (the tabs v1.x .. v10.x). v10's loop
// stops before the file's last pair (Krunker's file ends with "TRICKSHOT MUSIC", which has no
// version and is skipped here anyway), so the last update of kn-versions.txt is kept.
static int is_word_char(const char c) { return isalnum((unsigned char) c) || c == '_'; }

static int changelog_title(const char *line, const size_t len, const char **title, size_t *title_len) {
    size_t i = 0;
    while (i < len && isspace((unsigned char) line[i])) i++;
    if (len - i < 3 || strncmp(line + i, "== ", 3)) return 0;
    const char *t = line + i + 3;
    const size_t tl = len - i - 3;
    int ok = 0;
    for (size_t k = 0; k + 6 <= tl && !ok; k++) { // .*?\bUPDATE
        if (!strncmp(t + k, "UPDATE", 6) && (k == 0 || !is_word_char(t[k - 1]))) ok = 1;
    }
    if (!ok && tl >= 6 && !strncmp(t, "UPADTE", 6)) ok = 1;
    if (!ok && tl >= 7 && !strncmp(t, "INITIAL", 7)) ok = 1;
    if (!ok && tl >= 9 && !strncmp(t, "TRICKSHOT", 9) && (tl == 9 || !is_word_char(t[9]))) ok = 1;
    if (!ok) return 0;
    for (size_t k = tl; k >= 3; k--) { // (.*) == : greedy up to the last " ==" on the line
        if (!strncmp(t + k - 3, " ==", 3)) {
            *title = t;
            *title_len = k - 3;
            return 1;
        }
    }
    return 0;
}

typedef struct {
    char version[24];        // "1.2.0"
    int major;               // the tab: parseInt(version)
    const char *body;        // the update's text up to the next title
    size_t body_len;
} LogEntry;

static struct {
    char *text;
    LogEntry *log;
    int count;
    int tab;                 // windows[34].tabIndex: the major version shown
    int ad_hidden;           // v10's gen hides #updateAd once the What's New view was shown
    int ad_shown;            // #updateAd's state as last written (-1 unknown)
    int viewed;              // lastViewedUpdate == the version (-1: read it again)
    // the brain's news (GET /social/news)
    cJSON *news;             // array of HTML strings, NULL = not fetched
    int news_inflight, news_fresh, scenario;
} L = {.tab = -1, .ad_shown = -1, .viewed = -1};

static char *read_text(const char *path) {
    unsigned char *data = NULL;
    size_t length = 0;
    if (read_file(path, &data, &length) != 0) return NULL;
    char *t = realloc(data, length + 1);
    if (!t) {
        free(data);
        return NULL;
    }
    t[length] = 0;
    return t;
}

// <page dir>/windows/<name> (web: the preloaded /ui/windows)
static char *windows_file(const char *name) {
#ifdef __EMSCRIPTEN__
    char *path = concat("/ui/windows/", name);
#else
    const char *dir = getenv("KRUNKNATIVE_UI_DIR");
    char *base = dir && *dir ? concat(dir, "/windows/") : concat(ui_default_path(), "windows/");
    char *path = concat(base, name);
    free(base);
#endif
    char *text = read_text(path);
    free(path);
    return text;
}

static void changelog_load(void) {
    if (L.text) return;
    L.text = windows_file("kn-versions.txt"); // Krunker Box Office's changelog
    if (!L.text) L.text = windows_file("versions.txt");
    if (!L.text) return;
    typedef struct { const char *title; size_t title_len; const char *start, *end; } Hdr;
    Hdr *hdrs = NULL;
    size_t count = 0, cap = 0;
    const char *text = L.text, *tend = text + strlen(text);
    for (const char *p = text; p < tend;) {
        const char *nl = memchr(p, '\n', (size_t) (tend - p));
        const char *le = nl ? nl : tend;
        Hdr h;
        if (changelog_title(p, (size_t) (le - p), &h.title, &h.title_len)) {
            if (count == cap) {
                cap = cap ? cap * 2 : 64;
                Hdr *grown = realloc(hdrs, cap * sizeof(Hdr));
                if (!grown) break;
                hdrs = grown;
            }
            h.start = p;
            h.end = h.title + h.title_len + 3;
            hdrs[count++] = h;
        }
        p = nl ? nl + 1 : tend;
    }
    L.log = calloc(count ? count : 1, sizeof(LogEntry));
    for (size_t i = 0; L.log && i < count; i++) {
        const char *v = hdrs[i].title, *ve = v + hdrs[i].title_len;
        while (v < ve && !(isdigit((unsigned char) *v) || *v == '.')) v++;
        size_t k = 0;
        LogEntry *e = &L.log[L.count];
        while (v < ve && (isdigit((unsigned char) *v) || *v == '.') && k + 1 < sizeof(e->version)) e->version[k++] = *v++;
        e->version[k] = 0;
        if (!k || !isdigit((unsigned char) e->version[0])) continue;
        e->major = atoi(e->version);
        e->body = hdrs[i].end;
        e->body_len = (size_t) ((i + 1 < count ? hdrs[i + 1].start : tend) - hdrs[i].end);
        if (e->major > L.tab) L.tab = e->major; // parseInt(Object.keys(parsedLog).slice(-1))
        L.count++;
    }
    free(hdrs);
}

// config.gameVersion: the newest update of the changelog ("" when there is none)
const char *v10_news_version(void) {
    changelog_load();
    return L.count ? L.log[0].version : "";
}

static int update_viewed(void) {
    if (L.viewed < 0) {
        char seen[32] = "";
        html_storage_get("lastViewedUpdate", seen, sizeof(seen));
        L.viewed = !strcmp(seen, v10_news_version());
    }
    return L.viewed;
}

// One trimmed line of an update body: [a, b)
static int next_line(const char **p, const char *end, const char **a, const char **b) {
    while (*p < end) {
        const char *nl = memchr(*p, '\n', (size_t) (end - *p));
        const char *le = nl ? nl : end;
        *a = *p;
        *b = le;
        *p = nl ? nl + 1 : end;
        while (*a < *b && isspace((unsigned char) **a)) (*a)++;
        while (*b > *a && isspace((unsigned char) (*b)[-1])) (*b)--;
        if (*b > *a) return 1;
    }
    return 0;
}

static void news_expire(void *user) {
    (void) user;
    L.news_fresh = 0;
}

static void on_news(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    (void) length;
    L.news_inflight = 0;
    cJSON *res = status == 200 ? cJSON_Parse(body) : NULL;
    const cJSON *news = cJSON_GetObjectItem(cJSON_GetObjectItem(res, "data"), "news");
    cJSON_Delete(L.news);
    L.news = cJSON_CreateArray();
    const cJSON *n;
    cJSON_ArrayForEach(n, news) { // .filter(n => n && n.length > 0)
        if (cJSON_IsString(n) && n->valuestring[0]) cJSON_AddItemToArray(L.news, cJSON_CreateString(n->valuestring));
    }
    cJSON_Delete(res);
    L.news_fresh = 1;
    menu_log("news: %d item(s) (HTTP %d)", cJSON_GetArraySize(L.news), status);
    html_timer_add(NEWS_TTL_MS, news_expire, NULL);
    if (cJSON_GetArraySize(L.news) > 0) menu_rerender_if_open(35);
}

static void news_fetch(void) {
    if (L.scenario || L.news_inflight || L.news_fresh || backend_available() != 1) return;
    L.news_inflight = 1;
    if (!backend_http_get("/social/news", on_news, NULL)) L.news_inflight = 0;
}

// KN addition: the news items as v10's hub News tab draws them (svelte-1ult4vg)
static void news_cards(StrBuf *out) {
    if (cJSON_GetArraySize(L.news) <= 0) return;
    sb_append(out, "<div class=\"news-list svelte-1ult4vg\" style=\"margin-bottom:15px;\">");
    const cJSON *n;
    cJSON_ArrayForEach(n, L.news) {
        sb_append(out, "<div class=\"news-item svelte-1ult4vg\">");
        sb_append(out, n->valuestring); // markup the brain wrote (escaped text and its link)
        sb_append(out, "</div>");
    }
    sb_append(out, "</div>");
}

char *v10_news_changelog_gen(void) {
    changelog_load();
    news_fetch();
    StrBuf out = {0};
    if (!L.text) {
        sb_append(&out, "Loading...");
        return sb_finish(&out);
    }
    const char *version = v10_news_version();
    if (!update_viewed()) { // "Whats New in v1.2.0 ?"
        L.ad_hidden = 1;     // updateAd.style.display = "none"
        HtmlElem ad = html_dom_by_id("updateAd");
        if (ad) html_dom_set_display(ad, "none");
        L.ad_shown = 0;
        char bg[128];
        ui_img_url("img/backgrounds/whatsnew.png", bg, sizeof(bg));
        sb_appendf(&out, "<div class='logVersionHed' style='background:url(%s);'>Whats New in v", bg);
        sb_append_esc(&out, version);
        sb_append(&out, " ?</div>");
        news_cards(&out);
        int open = 0;
        if (L.count) {
            const char *p = L.log[0].body, *end = p + L.log[0].body_len, *a, *b;
            while (next_line(&p, end, &a, &b)) {
                if (*a == '*') {
                    if (open) sb_append(&out, "</div>");
                    open = 1;
                    sb_append(&out, "<div class='mapListCat'>");
                    sb_append_n(&out, a + 1, (size_t) (b - a - 1));
                    sb_append(&out, "</div><div class='setBodH' style='padding-top:20px;padding-bottom:20px;'>");
                } else {
                    const char *t = a + 1;
                    if (t < b && *t == ' ') t++;
                    if (*a == '@') {
                        sb_append(&out, "<img class='logImg' src='");
                        sb_append_n(&out, t, (size_t) (b - t));
                        sb_append(&out, "'><div></div>");
                    } else {
                        sb_append(&out, "- <span style='color:rgba(255,255,255,0.6);line-height: 1.8;'>");
                        sb_append_n(&out, t, (size_t) (b - t));
                        sb_append(&out, "</span><div></div>");
                    }
                }
            }
        }
        sb_append(&out, "</div><div class='viewAllUpdates' onclick='windows[34].viewAll()'>View All Updates</div>");
        return sb_finish(&out);
    }
    // every update of the selected major version, newest first; the latest one opened
    news_cards(&out);
    sb_append(&out, "<div style='margin-bottom:15px'><div style='height:20px;'></div><div class='bigMenTabs'>");
    int last = 1 << 30;
    for (;;) { // Object.keys(parsedLog).reverse(): the majors, highest first
        int major = -1;
        for (int i = 0; i < L.count; i++)
            if (L.log[i].major < last && L.log[i].major > major) major = L.log[i].major;
        if (major < 0) break;
        sb_appendf(&out, "<div class='bigMenTab%s' onclick='windows[34].switchTab(%d)'>v%d.x</div>", L.tab == major ? " bigMenTabA" : "", major, major);
        last = major;
    }
    sb_append(&out, "</div></div><div id='mailList'>");
    for (int i = 0; i < L.count; i++) {
        if (L.log[i].major != L.tab) continue;
        const int is_latest = i == 0; // latestUpd: the newest update of the newest major
        sb_appendf(&out, "<div class='mailObj' onclick='windows[34].toggleLog(this)'><i class='material-icons' style='vertical-align:middle;margin-top:-10px;font-size:50px;float:right;margin-right:10px;color:#fff'>%s</i><div class='mailFrom' style='color: #fbc02d;'>%s %s</div><div style='margin-top:10px;%s'>",
                   is_latest ? "arrow_drop_up" : "arrow_drop_down", is_latest ? "LATEST UPDATE" : "UPDATE", L.log[i].version, is_latest ? "" : "display:none;");
        // fixChanges: "*" and "@" lines dropped, the first "- " removed, one <li> per line
        sb_append(&out, "<li class='changeText' style='color: rgba(255, 255, 255, 0.5);'>");
        const char *p = L.log[i].body, *end = p + L.log[i].body_len, *a, *b;
        int first = 1;
        while (next_line(&p, end, &a, &b)) {
            if (*a == '*' || *a == '@') continue;
            if (!first) sb_append(&out, "</li><li class=\"changeText\" style=\"color: rgba(255, 255, 255, 0.5);\">");
            first = 0;
            const char *dash = NULL;
            for (const char *q = a; q + 1 < b && !dash; q++)
                if (q[0] == '-' && q[1] == ' ') dash = q;
            if (dash) {
                sb_append_n(&out, a, (size_t) (dash - a));
                sb_append_n(&out, dash + 2, (size_t) (b - dash - 2));
            } else {
                sb_append_n(&out, a, (size_t) (b - a));
            }
        }
        sb_append(&out, "</li></div></div>");
    }
    sb_append(&out, "</div>");
    return sb_finish(&out);
}

// windows[34].toggleLog(obj): children[2] shown / hidden, children[0] the arrow.
void v10_news_toggle_log(const HtmlElem obj) {
    if (!obj) return;
    HtmlElem icon = html_dom_first_child(obj);
    HtmlElem from = icon ? html_dom_next(icon) : 0;
    HtmlElem body = from ? html_dom_next(from) : 0;
    if (!icon || !body) return;
    char disp[16] = "";
    const int hidden = html_dom_get_style(body, "display", disp, sizeof(disp)) >= 0 && !strcmp(disp, "none");
    html_dom_set_display(body, hidden ? "block" : "none");
    html_dom_set_text(icon, hidden ? "arrow_drop_up" : "arrow_drop_down");
}

// The menu's What's New item (v10: shown on login while lastViewedUpdate != the version, with
// " - v<version>" after its title; gen hides it again)
static void update_ad_tick(void) {
    HtmlElem ad = html_dom_by_id("updateAd");
    if (!ad) return;
    const int show = active_account() && !L.ad_hidden && v10_news_version()[0] && !update_viewed();
    if (show == L.ad_shown) return;
    L.ad_shown = show;
    html_dom_set_display(ad, show ? "" : "none");
    HtmlElem ver = html_dom_by_id("updateAdVersion");
    if (show && ver) {
        char text[32];
        snprintf(text, sizeof(text), "v%s", v10_news_version());
        html_dom_set_text(ver, text);
    }
}

// ================================================================ openGuide(): How to Krunker

// v10's sections (_66977) in order: key, sidebar name, unlockLevel (0 = none), the section's
// markup (its template string; \001 = the next image's src, \002 = the Basics video) and images
// (img/guide/<name>.png).
typedef struct {
    const char *key, *name;
    int unlock;
    const char *html;
    const char *imgs[3];
} GuideSection;

static const GuideSection k_guide[] = {
    {"how-to-play", "Basics", 0,
         "<div class=\"tutorialSectionIntro svelte-los391\"><span class=\"tutorialSectionHeader svelte-los391\">Krunker is a free-to-play first-person shooter game that offers a variety of game modes and weapons for you to quickly play with your friends, anywhere, anytime.</span> "
         "<span class=\"tutorialSectionText svelte-los391\">When you first open the game, you can simply click the main screen to start playing.</span> "
         "<span class=\"tutorialSectionText svelte-los391\">The default mode is \"Free for All\", so shoot everyone you see!</span> "
         "<span class=\"tutorialSectionText svelte-los391\">You have infinite bullets in Krunker. No need to worry about running out.</span> "
         "<span class=\"tutorialSectionText svelte-los391\">The default weapon is the Assault Rifle.</span></div> "
         "<div class=\"tutorialSectionBody svelte-los391\">\002 <div class=\"tutorialSectionControls svelte-los391\"><span class=\"tutorialSectionText svelte-los391\">These are the basic default controls (you can change them in settings):</span> "
         "<span class=\"tutorialSectionText svelte-los391\">W A S D - Movement</span> "
         "<span class=\"tutorialSectionText svelte-los391\">Left Click - Shoot</span> "
         "<span class=\"tutorialSectionText svelte-los391\">Right Click - Aim</span> "
         "<span class=\"tutorialSectionText svelte-los391\">Space - Jump</span> "
         "<span class=\"tutorialSectionText svelte-los391\">Shift - Crouch</span> "
         "<span class=\"tutorialSectionText svelte-los391\">1 2 3 - Killstreaks</span> "
         "<span class=\"tutorialSectionText svelte-los391\">E - Secondary Weapon</span> "
         "<span class=\"tutorialSectionText svelte-los391\">Q - Melee Weapon</span> "
         "<span class=\"tutorialSectionText svelte-los391\">R - Reload</span></div></div> "
         "<div class=\"tutorialSectionFooter svelte-los391\" style=\"margin-top: 20px;\"><span class=\"tutorialSectionText svelte-los391\">Once a match ends, you can vote on the next map and game mode.</span> "
         "<span class=\"tutorialSectionText svelte-los391\">For additional information, please check out the other sections on this page.</span></div>",
         {NULL}},
    {"inviting-friends", "Inviting Friends", 0,
         "<div class=\"tutorialSectionIntro svelte-los391\"><span class=\"tutorialSectionHeader svelte-los391\">Playing with friends in Krunker is quick and easy.</span> "
         "<span class=\"tutorialSectionText svelte-los391\">Once you are in a game, click the \"Invite\" button in the matchmaking bar to copy a link to your game.</span> "
         "<span class=\"tutorialSectionText svelte-los391\">Send this link to your friends, and they can join you in the game by clicking the \"Join\" button.</span></div> "
         "<div style=\"display: flex; width: 100%; justify-content: center;\"><img class=\"matchmakingBarImage svelte-los391\" src=\"\001\"></div> "
         "<div class=\"tutorialSectionIntro svelte-los391\"><span class=\"tutorialSectionText svelte-los391\">Alternatively, you can also send them the link to your game by directly copying the URL from your browser's address bar.</span></div> "
         "<div style=\"display: flex; width: 100%; justify-content: center;\"><img class=\"inviteFriendsURLImage svelte-los391\" src=\"\001\"></div>",
         {"invite_friends", "invite_friends_url"}},
    {"class-and-loadout", "Class and Loadout", 0,
         "<div class=\"tutorialSectionIntro svelte-los391\"><span class=\"tutorialSectionHeader svelte-los391\">Krunker features a lot of different classes, each with their own primary weapons, health and speed. Some classes can even wall jump!</span> "
         "<span class=\"tutorialSectionText svelte-los391\">You can change classes by clicking your loadout information, next to your character preview.</span> "
         "<span class=\"tutorialSectionText svelte-los391\">Change class by clicking the one you want to play.</span> "
         "<span class=\"tutorialSectionText svelte-los391\">Once you select a class, you can modify your loadout.</span> "
         "<span class=\"tutorialSectionText svelte-los391\">You can pick a secondary weapon, change attachments, reticles, charms and weapon skins.</span></div> "
         "<div style=\"display: flex; gap: 20px; width: 100%; justify-content: space-evenly;\"><img class=\"classAndLoadoutImage svelte-los391\" src=\"\001\"> "
         "<img class=\"classAndLoadoutImage svelte-los391\" src=\"\001\"> "
         "<img class=\"classAndLoadoutImage svelte-los391\" src=\"\001\"></div> "
         "<div class=\"tutorialSectionFooter svelte-los391\"><span class=\"tutorialSectionText svelte-los391\">Note that you have to respawn for the changes to take effect.</span> "
         "<span class=\"tutorialSectionText svelte-los391\">Try each class and find out which one suits you best.</span></div>",
         {"loadout", "classes", "weapons"}},
    {"customization", "Customization", 0,
         "<div class=\"tutorialSectionIntro svelte-los391\"><span class=\"tutorialSectionHeader svelte-los391\">A lot of different types of cosmetics exist in Krunker. You can acquire cosmetics by using the spin wheels in the shop.</span> "
         "<span class=\"tutorialSectionText svelte-los391\">You can change your appearance by clicking \"Customize\" next to your character preview.</span> "
         "<span class=\"tutorialSectionText svelte-los391\">Equip a variety of items, like hats, silly faces, backpacks and more. You can even own a pet!</span> "
         "<span class=\"tutorialSectionText svelte-los391\">You can also change your character's skin and hair color.</span></div> "
         "<div style=\"display: flex; gap: 20px; width: 100%; justify-content: space-evenly;\"><img class=\"classAndLoadoutImage svelte-los391\" src=\"\001\"> "
         "<img class=\"classAndLoadoutImage svelte-los391\" src=\"\001\"> "
         "<img class=\"classAndLoadoutImage svelte-los391\" src=\"\001\"></div> "
         "<div class=\"tutorialSectionFooter svelte-los391\"><span class=\"tutorialSectionText svelte-los391\">Your character preview will update accordingly, so you can visualize your new look.</span></div>",
         {"customize_click", "customize", "customize_result"}},
    {"spins", "Spin Wheels", 0,
         "<div class=\"tutorialSectionIntro svelte-los391\"><span class=\"tutorialSectionHeader svelte-los391\">Spin wheels are the primary way of earning cosmetics in Krunker.</span> "
         "<span class=\"tutorialSectionText svelte-los391\">There are multiple spin wheels available in the shop, each with increasingly rare rewards and costs.</span> "
         "<span class=\"tutorialSectionText svelte-los391\">You can spend currency to spin the wheels, or you can use \"Spin Tickets\" earned through the free and paid tiers of the battlepass.</span> "
         "<span class=\"tutorialSectionText svelte-los391\">The existing item rarities are \"Uncommon\", \"Rare\", \"Epic\", \"Legendary\", \"Relic\", \"Contraband\" and \"Unobtainable\".</span></div> "
         "<div style=\"display: flex; gap: 20px; width: 100%; justify-content: center;\"><img class=\"spinsImage svelte-los391\" src=\"\001\"> "
         "<img class=\"spinsImage svelte-los391\" src=\"\001\"></div> "
         "<div class=\"tutorialSectionFooter svelte-los391\"><span class=\"tutorialSectionText svelte-los391\">Available items renew every season, so if you really want a specific item, you should try to get it before the season ends.</span></div>",
         {"spins", "spin_reward"}},
    {"game-modes", "Game Modes", 0,
         "<div class=\"tutorialSectionIntro svelte-los391\"><span class=\"tutorialSectionHeader svelte-los391\">Krunker as a lot of different game modes to offer.</span> "
         "<span class=\"tutorialSectionText svelte-los391\">If you quickly want to move away from your current game mode, press the \"Quick Match\" button on the matchmaking bar.</span></div> "
         "<div style=\"display: flex; width: 100%; justify-content: center;\"><img class=\"matchmakingBarQuickMatchImage svelte-los391\" src=\"\001\"></div> "
         "<div class=\"tutorialSectionIntro svelte-los391\"><span class=\"tutorialSectionText svelte-los391\">Alternatively, you can click \"Find Game\" to navigate the existing games list, or \"Host Game\" to make your own.</span></div> "
         "<div style=\"display: flex; gap: 20px; width: 100%; justify-content: center;\"><img class=\"hostFindGameImage svelte-los391\" src=\"\001\"> "
         "<img class=\"hostGameSetupImage svelte-los391\" src=\"\001\"></div>",
         {"quick_match", "host_find_game", "host_game_setup"}},
    {"challenges", "Challenges", 0,
         "<div class=\"tutorialSectionIntro svelte-los391\"><span class=\"tutorialSectionHeader svelte-los391\">Challenges give players milestone goals to achieve. You can earn currency and special cosmetics by completing them.</span> "
         "<span class=\"tutorialSectionText svelte-los391\">There are challenges specific to each map and weapon.</span></div> "
         "<div style=\"display: flex; gap: 20px; width: 100%; justify-content: center;\"><img class=\"challengesImage svelte-los391\" src=\"\001\"> "
         "<img class=\"challengesImage svelte-los391\" src=\"\001\"></div>",
         {"map_challenges", "weapon_challenges"}},
    {"kill-streaks", "Kill Streaks", 0,
         "<div class=\"tutorialSectionIntro svelte-los391\"><span class=\"tutorialSectionHeader svelte-los391\">In Krunker, \"Killstreaks\" are special abilities that you can use whenever you get a specific amount of kills in a row.</span> "
         "<span class=\"tutorialSectionText svelte-los391\">There are 3 available slots in your loadout, and multiple killstreaks for each slot, ranging from air drop packages to nukes.</span> "
         "<span class=\"tutorialSectionText svelte-los391\">You can change your assigned killstreaks in the loadout menu.</span></div> "
         "<div style=\"display: flex; gap: 20px; width: 100%; justify-content: center;\"><img class=\"challengesImage svelte-los391\" src=\"\001\"> "
         "<img class=\"challengesImage svelte-los391\" src=\"\001\"></div>",
         {"streaks_find", "streaks_equip"}},
    {"ranked", "Ranked", 10,
         "<div class=\"tutorialSectionIntro svelte-los391\"><span class=\"tutorialSectionHeader svelte-los391\">Play competitive ranked matches against other players.</span> "
         "<span class=\"tutorialSectionText svelte-los391\">The game mode is 4v4 \"Hardpoint\", in a best of 3 format. Matches last around 10 to 15 minutes.</span> "
         "<span class=\"tutorialSectionText svelte-los391\">Play in a selected map pool tested and approved by the competitive Krunker community.</span> "
         "<span class=\"tutorialSectionText svelte-los391\">Climb through the ranks by winning matches, and earn special cosmetics at the end of each season.</span> "
         "<span class=\"tutorialSectionText svelte-los391\">There is a leaderboard where you can see the rank of the top players in the world.</span></div> "
         "<div style=\"display: flex; gap: 20px; width: 100%; justify-content: center;\"><img class=\"rankedImage svelte-los391\" src=\"\001\"> "
         "<img class=\"rankedImage svelte-los391\" src=\"\001\"></div>",
         {"ranked", "ranked_leaderboard"}},
    {"clans", "Clans", 20,
         "<div class=\"tutorialSectionIntro svelte-los391\"><span class=\"tutorialSectionHeader svelte-los391\">You and other players can group together to form a clan.</span> "
         "<span class=\"tutorialSectionText svelte-los391\">You can apply to a clan or create your own through your player profile. You can only have one clan at a time.</span> "
         "<span class=\"tutorialSectionText svelte-los391\">Clans have their own 4 letter tag, that will show up next to your name.</span> "
         "<span class=\"tutorialSectionText svelte-los391\">Once a clan is created, the clan commander can manage the clan through the clan page.</span></div> "
         "<div style=\"display: flex; width: 100%; gap: 20px; justify-content: space-evenly;\"><div style=\"display: flex; width: 40%; flex-direction: column; gap: 20px;\"><img class=\"clanCreateImage svelte-los391\" src=\"\001\"> "
         "<img class=\"clanCreateImage svelte-los391\" src=\"\001\"></div> "
         "<img class=\"clanManageImage svelte-los391\" src=\"\001\"></div>",
         {"clan_profile", "clan_create", "clan_manage"}},
    {"trading", "Trading", 10,
         "<div class=\"tutorialSectionIntro svelte-los391\"><span class=\"tutorialSectionHeader svelte-los391\">Trading is a huge part of Krunker and one of the most popular features.</span> "
         "<span class=\"tutorialSectionText svelte-los391\">You can quick sell items in exchange for currency, or list them for sale for specific values, so other players can search those items and buy them from you.</span> "
         "<span class=\"tutorialSectionText svelte-los391\">Head to \"Social and Trading\" and click the \"Market\" button.</span> "
         "<span class=\"tutorialSectionText svelte-los391\">Once you reach level 10, you can also send trade offers directly to other players. You can do so in the market or in-game.</span></div> "
         "<div style=\"display: flex; gap: 20px; width: 100%; justify-content: center;\"><img class=\"tradingImage svelte-los391\" src=\"\001\"> "
         "<img class=\"tradingImage svelte-los391\" src=\"\001\"></div>",
         {"market", "trading_ui"}},
    {"challenge-mode", "Challenge Mode", 30,
         "<div class=\"tutorialSectionIntro svelte-los391\"><span class=\"tutorialSectionHeader svelte-los391\">Challenge Mode is a special game setting reserved for more veteran players.</span> "
         "<span class=\"tutorialSectionText svelte-los391\">Enabling Challenge Mode will half your maximum health as well as disable automatic health regeneration.</span> "
         "<span class=\"tutorialSectionText svelte-los391\">In exchange, you will gain 50% more currency at the end of the match.</span> "
         "<span class=\"tutorialSectionText svelte-los391\">To activate Challenge Mode, enable \"Advanced\" settings and activate the \"Challenge Mode\" switch in the settings menu.</span></div> "
         "<div style=\"display: flex; gap: 20px; width: 100%; justify-content: center;\"><img class=\"challengeModeImage svelte-los391\" src=\"\001\"></div> "
         "<div style=\"display: flex; gap: 20px; width: 100%; justify-content: center; align-items: center;\"><span class=\"tutorialSectionText svelte-los391\">You will know you are in challenge mode when the skull icon is shown on the bottom left of the screen, above your health value.</span> "
         "<img class=\"challengeModeDifferenceImage svelte-los391\" src=\"\001\"> "
         "<img class=\"challengeModeDifferenceImage svelte-los391\" src=\"\001\"></div> "
         "<div class=\"tutorialSectionFooter svelte-los391\"><span class=\"tutorialSectionText svelte-los391\">This feature has no effect on ranked matches.</span></div>",
         {"challenge_mode", "challenge_mode_off", "challenge_mode_on"}},
};
#define GUIDE_COUNT ((int) (sizeof(k_guide) / sizeof(k_guide[0])))

static int g_guide = 0; // the shown section (initialSection, default "how-to-play")

static int guide_find(const char *key) {
    for (int i = 0; key && i < GUIDE_COUNT; i++)
        if (!strcmp(k_guide[i].key, key)) return i;
    return -1;
}

// !unlockLevel || acc && acc.level >= unlockLevel
static int guide_unlocked(const int i) {
    const AccountInfo *a = active_account();
    return !k_guide[i].unlock || (a && a->level >= k_guide[i].unlock);
}

static void guide_video(StrBuf *out) {
    char url[512];
    html_dom_asset_url("videos/tutorial/basics.mp4", url, sizeof(url));
    sb_append(out, "<video style=\"width: 700px;\" preload=\"auto\" src=\"");
    sb_append_esc(out, url);
    sb_append(out, "\" autoplay=\"\" loop=\"\" playsinline=\"\"");
#ifndef __EMSCRIPTEN__
    // the native engine plays the video's .knv frame sidecar (engine/video.h); its first frame
    // stands in while the sidecar is missing (KN addition: v10 has no poster)
    sb_append(out, " poster=\"");
    sb_img(out, "img/guide/kn_basics_poster.jpg");
    sb_append(out, "\"");
#endif
    sb_append(out, "></video>");
}

static char *guide_markup(void) {
    StrBuf out = {0};
    sb_append(&out, "<div class=\"tutorialPopup svelte-los391\"><div class=\"tutorialHeader svelte-los391\">How to Krunker</div> "
                    "<div class=\"tutorialContent svelte-los391\"><div class=\"tutorialSidebar svelte-los391\">");
    for (int i = 0; i < GUIDE_COUNT; i++) {
        const int active = i == g_guide, unlocked = guide_unlocked(i);
        sb_appendf(&out, "<div class=\"tutorialNavItem %s %s svelte-los391\" onclick=\"knGuideSection('%s')\">"
                         "<div class=\"navItemContent svelte-los391\"><span class=\"navItemText %s svelte-los391\">%s</span>",
                   active ? "active" : "", unlocked ? "" : "locked", k_guide[i].key, active ? "active" : "", k_guide[i].name);
        if (k_guide[i].unlock && !unlocked)
            sb_appendf(&out, " <div class=\"lockInfo svelte-los391\"><i class=\"material-icons lockIcon svelte-los391\">lock</i> "
                             "<span class=\"unlockText svelte-los391\">Unlock at level %d</span></div>", k_guide[i].unlock);
        else if (k_guide[i].unlock)
            sb_append(&out, " <div class=\"unlockInfo svelte-los391\"><i class=\"material-icons unlockIcon svelte-los391\">check_circle</i> "
                            "<span class=\"unlockText svelte-los391\">Feature unlocked</span></div>");
        sb_append(&out, "</div></div>");
    }
    sb_append(&out, "</div> <div class=\"tutorialSectionContent svelte-los391\">");
    if (g_guide >= 0 && g_guide < GUIDE_COUNT) {
        const GuideSection *s = &k_guide[g_guide];
        int img = 0;
        for (const char *p = s->html; *p; p++) {
            if (*p == '\001') {
                char path[64];
                snprintf(path, sizeof(path), "img/guide/%s.png", img < 3 && s->imgs[img] ? s->imgs[img] : "");
                img++;
                sb_img(&out, path);
            } else if (*p == '\002') {
                guide_video(&out);
            } else {
                const char *q = p;
                while (q[1] && q[1] != '\001' && q[1] != '\002') q++;
                sb_append_n(&out, p, (size_t) (q - p + 1));
                p = q;
            }
        }
    } else {
        sb_append(&out, "<div class=\"tutorialSectionHeader svelte-los391\">How did you get here?</div>");
    }
    sb_append(&out, "</div></div></div>");
    return sb_finish(&out);
}

static void guide_render(void) {
    char *html = guide_markup();
    HtmlElem pop = html_dom_by_id("genericPop");
    if (popup_open("tutorialPopup") && pop) {
        // a section change keeps the popup (Svelte updates it in place): the sidebar's scroll stays
        HtmlScroll side = {0};
        HtmlElem bar = html_dom_query(pop, ".tutorialSidebar");
        const int had = bar && html_dom_get_scroll(bar, &side);
        html_dom_set_html(pop, html);
        bar = html_dom_query(pop, ".tutorialSidebar");
        if (had && bar) html_dom_set_scroll(bar, side.left, side.top);
    } else {
        generic_popup("tutorialPopup", html);
    }
    free(html);
}

// window.openGuide(section)
void v10_news_open_guide(const char *section) {
    g_guide = section && *section ? guide_find(section) : 0; // an unknown key: "How did you get here?"
    generic_popup("tutorialPopup", "");
    guide_render();
}

// ================================================================ openCommunityMenu(): Community and Events

static struct {
    cJSON *events, *polls; // the last /creators/events answer (arrays), NULL before one
    int loading;           // v10's _66849: true until the request settled
    int serial;            // the open popup's request (late answers of an older one are dropped)
} E;

// v10's socials footer (_66850)
static const struct {
    const char *name, *icon, *type, *href, *color;
} k_socials[] = {
    {"TikTok", "tiktok", "material", "https://www.tiktok.com/@krunker.io", "#fff"},
    {"Discord", "img/social/discord.png", "img", "http://discord.gg/krunker", "#5865f2"},
    {"Reddit", "img/social/reddit.png", "img", "https://www.reddit.com/r/KrunkerIO/", "#ff4500"},
    {"Twitch", "twitch", "svg", "https://www.twitch.tv/directory/game/Krunker", "#9146ff"},
    {"YouTube", "img/youtube.png", "img", "https://www.youtube.com/channel/UCqH34FGuOae5o-wO2wOa38A", "#ff0000"},
};

#define TWITCH_PATH "M11.571 4.714h1.715v5.143H11.57zm4.715 0H18v5.143h-1.714zM6 0L1.714 4.286v15.428h5.143V24l4.286-4.286h3.428L22.286 " \
                    "12V0zm14.571 11.143l-3.428 3.428h-3.429l-3 3v-3H6.857V1.714h13.714Z"

// A link of the popup: v10's <a target="_blank" href>; here the URL rides in data-kn-url and
// knCommunityLink() follows it (a game link plays the map in this client, the rest opens like
// every external link: ui_open_external).
static void link_attrs(StrBuf *out, const char *url) {
    sb_append(out, " href=\"#\" data-kn-url=\"");
    sb_append_esc(out, url);
    sb_append(out, "\" onclick=\"knCommunityLink()\"");
}

// new Date(s).toLocaleDateString("en-US", {weekday: "short", month: "short", day: "numeric",
// hour: "numeric", minute: "2-digit"}): "Wed, Oct 7, 11:11 AM", local time (Chrome puts U+202F
// before AM/PM; a plain space here, the menu fonts have no U+202F)
static void event_date(StrBuf *out, const char *iso) {
    const long long t = parse_iso(iso);
    if (t < 0) {
        sb_append(out, "Invalid Date");
        return;
    }
    const time_t tt = (time_t) t;
    const struct tm *tm = localtime(&tt);
    if (!tm) return;
    char day[32];
    strftime(day, sizeof(day), "%a, %b ", tm);
    const int h12 = tm->tm_hour % 12 ? tm->tm_hour % 12 : 12;
    sb_appendf(out, "%s%d, %d:%02d %s", day, tm->tm_mday, h12, tm->tm_min, tm->tm_hour < 12 ? "AM" : "PM");
}

// now >= start && (duration_hours ? now < start + duration && !ended_at : !ended_at)
static int event_live(const cJSON *e) {
    const long long now = (long long) time(NULL), start = parse_iso(jstr(e, "start_date"));
    const cJSON *dur = cJSON_GetObjectItem(e, "duration_hours");
    const int ended = jstr(e, "ended_at")[0] != 0;
    if (start < 0 || now < start) return 0;
    if (cJSON_IsNumber(dur) && dur->valuedouble != 0.0) return now < start + (long long) (3600.0 * dur->valuedouble) && !ended;
    return !ended;
}

// a CSS url() value: no quotes, parentheses, backslashes or whitespace
static int css_url_ok(const char *u) {
    if (!u || !u[0]) return 0;
    for (const char *p = u; *p; p++)
        if (*p == '"' || *p == '\'' || *p == '(' || *p == ')' || *p == '\\' || isspace((unsigned char) *p)) return 0;
    return 1;
}

static void event_card(StrBuf *out, const cJSON *e) {
    const cJSON *links = cJSON_GetObjectItem(e, "links");
    const char *first = cJSON_GetArraySize(links) > 0 ? jstr(cJSON_GetArrayItem(links, 0), "url") : "";
    sb_append(out, "<div class=\"eventCard svelte-1ley7u4\">");
    const char *banner = jstr(e, "banner_url");
#ifndef __EMSCRIPTEN__
    // the native engine reads page and asset files only: a remote banner would stay an empty box
    if (!strncmp(banner, "http://", 7) || !strncmp(banner, "https://", 8) || !strncmp(banner, "//", 2)) banner = "";
#endif
    if (css_url_ok(banner)) {
        if (first[0]) {
            sb_append(out, "<a class=\"eventBanner svelte-1ley7u4\" target=\"_blank\" rel=\"noopener noreferrer\" style=\"background-image:url(");
            sb_append_esc(out, banner);
            sb_append(out, ")\"");
            link_attrs(out, first);
            sb_append(out, "></a>");
        } else {
            sb_append(out, "<div class=\"eventBanner svelte-1ley7u4\" style=\"background-image:url(");
            sb_append_esc(out, banner);
            sb_append(out, ")\"></div>");
        }
    }
    sb_append(out, " <div class=\"eventContent svelte-1ley7u4\"><div class=\"eventMeta svelte-1ley7u4\"><span class=\"eventDate svelte-1ley7u4\">");
    event_date(out, jstr(e, "start_date"));
    sb_append(out, "</span> ");
    if (event_live(e)) sb_append(out, "<span class=\"eventLive svelte-1ley7u4\">LIVE</span>");
    sb_append(out, " ");
    const cJSON *dur = cJSON_GetObjectItem(e, "duration_hours");
    if (cJSON_IsNumber(dur) && dur->valuedouble != 0.0) sb_appendf(out, "<span class=\"eventDuration svelte-1ley7u4\">%gh</span>", dur->valuedouble);
    sb_append(out, "</div> ");
    if (first[0]) {
        sb_append(out, "<a class=\"eventTitle eventTitleLink svelte-1ley7u4\" target=\"_blank\" rel=\"noopener noreferrer\"");
        link_attrs(out, first);
        sb_append(out, ">");
        sb_append_esc(out, jstr(e, "title"));
        sb_append(out, "</a>");
    } else {
        sb_append(out, "<div class=\"eventTitle svelte-1ley7u4\">");
        sb_append_esc(out, jstr(e, "title"));
        sb_append(out, "</div>");
    }
    sb_append(out, " <div class=\"eventDesc svelte-1ley7u4\">");
    sb_append_esc(out, jstr(e, "description"));
    sb_append(out, "</div> ");
    const cJSON *tags = cJSON_GetObjectItem(e, "tags"), *t;
    if (cJSON_GetArraySize(tags) > 0) {
        sb_append(out, "<div class=\"eventTags svelte-1ley7u4\">");
        cJSON_ArrayForEach(t, tags) {
            sb_append(out, "<span class=\"eventTagChip svelte-1ley7u4\">");
            sb_append_esc(out, cJSON_IsString(t) ? t->valuestring : "");
            sb_append(out, "</span>");
        }
        sb_append(out, "</div>");
    }
    sb_append(out, " <div class=\"eventFooter svelte-1ley7u4\"><span class=\"eventCreator svelte-1ley7u4\">by ");
    sb_append_esc(out, jstr(e, "player_name"));
    sb_append(out, "</span> ");
    if (cJSON_GetArraySize(links) > 0) {
        sb_append(out, "<div class=\"eventLinks svelte-1ley7u4\">");
        const cJSON *l;
        cJSON_ArrayForEach(l, links) {
            const char *url = jstr(l, "url"), *label = jstr(l, "label");
            sb_append(out, "<a class=\"eventLink svelte-1ley7u4\" target=\"_blank\" rel=\"noopener noreferrer\"");
            link_attrs(out, url);
            sb_append(out, ">");
            sb_append_esc(out, label[0] ? label : url); // link.label || link.url
            sb_append(out, "</a>");
        }
        sb_append(out, "</div>");
    }
    sb_append(out, "</div></div></div>");
}

// v10's ends_in formatting: "1d 21h" / "3h 5m" / "12m" (at least 1m)
static void poll_closes(StrBuf *out, const double secs) {
    const long long s = secs > 0 ? (long long) secs : 0;
    const long long d = s / 86400, h = s % 86400 / 3600, m = s % 3600 / 60;
    if (d > 0) sb_appendf(out, "%lldd %lldh", d, h);
    else if (h > 0) sb_appendf(out, "%lldh %lldm", h, m);
    else sb_appendf(out, "%lldm", m > 1 ? m : 1);
}

static void poll_card(StrBuf *out, const cJSON *p) {
    sb_appendf(out, "<div class=\"pollCard svelte-1ley7u4\"><div class=\"pollMeta svelte-1ley7u4\"><span class=\"pollBadge svelte-1ley7u4\">"
                    "%s</span> <span class=\"pollCloses svelte-1ley7u4\">%s ",
               I18N("windows.community.polls.badge", "Community Poll"), I18N("windows.community.polls.closesIn", "Closes in"));
    const cJSON *ends = cJSON_GetObjectItem(p, "ends_in");
    poll_closes(out, cJSON_IsNumber(ends) ? ends->valuedouble : 0.0);
    sb_append(out, "</span></div> <div class=\"pollTitle svelte-1ley7u4\">");
    sb_append_esc(out, jstr(p, "title"));
    sb_append(out, "</div> ");
    if (jstr(p, "description")[0]) {
        sb_append(out, "<div class=\"pollDesc svelte-1ley7u4\">");
        sb_append_esc(out, jstr(p, "description"));
        sb_append(out, "</div>");
    }
    // v10: COMMUNITY_URI + "/polls/" + id (community.krunker.io); the brain gives each poll its url
    sb_append(out, " <a class=\"pollVote svelte-1ley7u4\" target=\"_blank\" rel=\"noopener noreferrer\"");
    link_attrs(out, jstr(p, "url"));
    sb_appendf(out, ">%s</a></div>", I18N("windows.community.polls.vote", "Vote now"));
}

static char *community_markup(void) {
    StrBuf out = {0};
    sb_append(&out, "<div class=\"communityWrapper svelte-1ley7u4\"><div class=\"communityScrollArea svelte-1ley7u4\">"
                    "<div class=\"guideHeader\">Community and Events</div> <div class=\"eventsSection svelte-1ley7u4\">");
    if (cJSON_GetArraySize(E.polls) > 0) {
        sb_append(&out, "<div class=\"pollsList svelte-1ley7u4\">");
        const cJSON *p;
        cJSON_ArrayForEach(p, E.polls) poll_card(&out, p);
        sb_append(&out, "</div>");
    }
    sb_append(&out, " ");
    if (E.loading) {
        sb_append(&out, "<div class=\"eventsEmpty svelte-1ley7u4\">Loading events...</div>");
    } else if (cJSON_GetArraySize(E.events) <= 0) {
        sb_append(&out, "<div class=\"eventsEmpty svelte-1ley7u4\">No upcoming events</div>");
    } else {
        sb_append(&out, "<div class=\"eventsList svelte-1ley7u4\">");
        const cJSON *e;
        cJSON_ArrayForEach(e, E.events) event_card(&out, e);
        sb_append(&out, "</div>");
    }
    sb_append(&out, "</div></div> <div class=\"socialsStickyFooter svelte-1ley7u4\"><div class=\"socialsFooterText svelte-1ley7u4\">"
                    "Krunker is built by its community. Join us on your favorite platform.</div> <div class=\"socialsGrid svelte-1ley7u4\">");
    for (size_t i = 0; i < sizeof(k_socials) / sizeof(k_socials[0]); i++) {
        sb_append(&out, "<a class=\"socialCard svelte-1ley7u4\" target=\"_blank\" rel=\"noopener noreferrer\"");
        link_attrs(&out, k_socials[i].href);
        sb_append(&out, "><div class=\"socialIcon svelte-1ley7u4\">");
        if (!strcmp(k_socials[i].type, "material")) {
            sb_appendf(&out, "<span class=\"material-icons-outlined\" style=\"font-size:32px;color:%s;\">%s</span>", k_socials[i].color, k_socials[i].icon);
        } else if (!strcmp(k_socials[i].type, "svg")) {
#ifdef __EMSCRIPTEN__
            sb_appendf(&out, "<svg width=\"28\" height=\"28\" viewBox=\"0 0 24 24\" fill=\"%s\"><path d=\"" TWITCH_PATH "\"></path></svg>", k_socials[i].color);
#else
            // the engine draws SVG images, not inline <svg>: the same icon as an image (KN deviation)
            sb_append(&out, "<img style=\"width:28px;height:28px;\" alt=\"Twitch\" src=\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' "
                            "width='28' height='28' viewBox='0 0 24 24' fill='%239146ff'%3E%3Cpath d='" TWITCH_PATH "'/%3E%3C/svg%3E\">");
#endif
        } else {
            sb_append(&out, "<img style=\"width:32px;height:32px;\" src=\"");
            sb_img(&out, k_socials[i].icon);
            sb_appendf(&out, "\" alt=\"%s\">", k_socials[i].name);
        }
        sb_appendf(&out, "</div> <span class=\"socialName svelte-1ley7u4\">%s</span></a>", k_socials[i].name);
    }
    sb_append(&out, "</div></div></div>");
    return sb_finish(&out);
}

static void community_render(void) {
    if (!popup_open("communityPopup")) return;
    HtmlElem pop = html_dom_by_id("genericPop");
    char *html = community_markup();
    if (pop) html_dom_set_html(pop, html);
    free(html);
}

static void on_events(void *user, const int status, const char *body, const size_t length) {
    (void) length;
    if ((int) (intptr_t) user != E.serial) return; // the popup was closed / opened again since
    cJSON *res = status == 200 ? cJSON_Parse(body) : NULL;
    const cJSON *data = cJSON_GetObjectItem(res, "data");
    if (data) { // res.data && (events = data.events || [], polls = data.polls || [])
        const cJSON *ev = cJSON_GetObjectItem(data, "events"), *po = cJSON_GetObjectItem(data, "polls");
        cJSON_Delete(E.events);
        cJSON_Delete(E.polls);
        E.events = cJSON_IsArray(ev) ? cJSON_Duplicate(ev, 1) : cJSON_CreateArray();
        E.polls = cJSON_IsArray(po) ? cJSON_Duplicate(po, 1) : cJSON_CreateArray();
    }
    cJSON_Delete(res);
    E.loading = 0;
    community_render();
}

// the language changed: the open Community popup again (its poll cards are i18n strings)
static void on_locale(void *user) {
    (void) user;
    community_render();
}

// window.openCommunityMenu(): the component fetches its events on every mount
void v10_news_open_community(void) {
    i18n_on_change(on_locale, NULL);
    E.serial++;
    E.loading = 0;
    if (!L.scenario) {
        cJSON_Delete(E.events);
        cJSON_Delete(E.polls);
        E.events = cJSON_CreateArray();
        E.polls = cJSON_CreateArray();
        // without a brain the request fails at once: v10's catch, then loading = false
        if (backend_available() == 1) E.loading = backend_http_get("/creators/events", on_events, (void *) (intptr_t) E.serial) != 0;
    }
    char *html = community_markup();
    generic_popup("communityPopup", html);
    free(html);
}

// knCommunityLink(): an event / poll / social link (data-kn-url)
static void community_link(const HtmlEvent *ev) {
    char url[1024] = "";
    HtmlElem a = ev && ev->target ? html_dom_closest(ev->target, "[data-kn-url]") : 0;
    if (!a || html_dom_get_attr(a, "data-kn-url", url, sizeof(url)) < 0 || !url[0]) return;
    menu_log("community link %s", url);
    // the game page's own ?play=<map> link (the brain's featured maps): play it here (playMap)
    const char *q = !strncmp(url, "/?", 2) ? url + 2 : !strncmp(url, "?", 1) ? url + 1 : NULL;
    const char *play = q && !strncmp(q, "play=", 5) ? q + 5 : NULL;
    if (play) {
        char name[256];
        size_t o = 0;
        for (const char *p = play; *p && *p != '&' && *p != '#' && o + 1 < sizeof(name); p++) {
            if (*p == '%' && isxdigit((unsigned char) p[1]) && isxdigit((unsigned char) p[2])) {
                char hex[3] = {p[1], p[2], 0};
                name[o++] = (char) strtol(hex, NULL, 16);
                p += 2;
            } else {
                name[o++] = *p == '+' ? ' ' : *p;
            }
        }
        name[o] = 0;
        ui_loadout_clear_pops();
        const HtmlArg arg = {HTML_ARG_STR, 0.0, name};
        html_ui_dispatch("playMap", ev, &arg, 1);
        return;
    }
    ui_open_external(url);
}

// ================================================================ the v10 menu items, handlers

// The v10 menu's Svelte items have no inline handlers in the reconstructed page: each gets the
// action v10's component gives it (playSelect + the action, playTick on hover; _50477).
static void menu_item_action(HtmlElem item, const char *action) {
    if (!item) return;
    char have[8];
    if (html_dom_get_attr(item, "onclick", have, sizeof(have)) >= 0) return; // wired by someone else
    char onclick[128];
    snprintf(onclick, sizeof(onclick), "playSelect(),%s", action);
    html_dom_set_attr(item, "onclick", onclick);
    html_dom_set_attr(item, "onmouseenter", "playTick()");
}

void v10_news_setup(void) {
    L.ad_shown = -1;
    L.ad_hidden = 0;
    L.viewed = -1;
    HtmlElem guide = html_dom_by_id("menuBtnGuide");
    if (!guide) return; // not the v10 page
    html_ui_add_stylesheet("css/kn-v10-news.css");
    v10_misc_popups_setup(); // the Svelte popups v10 mounts with the page (k-ID intro; css/kn-v10-popups.css)
    menu_item_action(html_dom_closest(guide, ".menuItem"), "openGuide()");
    menu_item_action(html_dom_query(html_dom_by_id("updateAd"), ".menuItem"), "showWindow(35)");
    HtmlElem community = html_dom_by_id("menuBtnSideCommunity");
    menu_item_action(community ? html_dom_closest(community, ".menuItem") : 0, "openCommunityMenu()");
    // the What's New title's version (_52131: "&nbsp;- v10.0.2"), which the static page lacks
    HtmlElem ad = html_dom_by_id("updateAd");
    HtmlElem title = ad ? html_dom_query(ad, ".menuItemTitle") : 0;
    if (title && !html_dom_by_id("updateAdVersion"))
        html_dom_insert_html(title, HTML_INSERT_BEFORE_END,
                             "<span class=\"whats-new-version svelte-159x57l\">&nbsp;- <span id=\"updateAdVersion\" class=\"svelte-159x57l\"></span></span>");
}

void v10_news_tick(void) {
    v10_misc_popups_tick(); // countdowns of the open mission briefing, lottery and survey
    update_ad_tick();
    // the What's New item is up: its news are fetched before the click (news_fetch: once per TTL)
    if (L.ad_shown == 1) news_fetch();
}

int v10_news_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    if (!name) return -1;
    const int misc = v10_misc_popups_handle(name, ev, args, argc); // knMisc*, openLottery, manageAccNames
    if (misc >= 0) return misc;
    const char *s0 = argc > 0 && args[0].kind == HTML_ARG_STR ? args[0].str : NULL;
    if (!strcmp(name, "openGuide") || !strcmp(name, "window.openGuide")) {
        v10_news_open_guide(s0);
        return 1;
    }
    if (!strcmp(name, "knGuideSection")) { // a sidebar item (locked ones open too, as in v10)
        const int i = guide_find(s0);
        if (i >= 0 && i != g_guide) {
            g_guide = i;
            guide_render();
        }
        return 1;
    }
    if (!strcmp(name, "openCommunityMenu") || !strcmp(name, "window.openCommunityMenu")) {
        v10_news_open_community();
        return 1;
    }
    if (!strcmp(name, "knCommunityLink")) {
        community_link(ev);
        return 0; // the <a href="#"> stays put
    }
    if (!strcmp(name, "windows.34.switchTab")) {
        if (argc > 0 && args[0].kind == HTML_ARG_NUM && (int) args[0].num != L.tab) {
            L.tab = (int) args[0].num;
            menu_rerender_if_open(35);
        }
        return 1;
    }
    if (!strcmp(name, "windows.34.viewAll")) { // lastViewedUpdate = the version; showWindow(35, true)
        html_storage_set("lastViewedUpdate", v10_news_version());
        L.viewed = 1;
        menu_show_window(35, 1);
        return 1;
    }
    return -1;
}

// --render-scenario state.community: {events, polls, news, popups} as the brain would answer, no requests
void v10_news_scenario(const cJSON *community) {
    if (!cJSON_IsObject(community)) return;
    L.scenario = 1;
    v10_misc_popups_scenario(cJSON_GetObjectItem(community, "popups"));
    const cJSON *ev = cJSON_GetObjectItem(community, "events"), *po = cJSON_GetObjectItem(community, "polls");
    const cJSON *news = cJSON_GetObjectItem(community, "news");
    cJSON_Delete(E.events);
    cJSON_Delete(E.polls);
    cJSON_Delete(L.news);
    E.events = cJSON_IsArray(ev) ? cJSON_Duplicate(ev, 1) : cJSON_CreateArray();
    E.polls = cJSON_IsArray(po) ? cJSON_Duplicate(po, 1) : cJSON_CreateArray();
    L.news = cJSON_IsArray(news) ? cJSON_Duplicate(news, 1) : cJSON_CreateArray();
}
