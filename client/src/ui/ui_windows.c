// Menu window markup: loads ui/windows/windows.json + the captured window files and
// generates what krunknative.js's WINDOWS[id].gen() wrote into #menuWindow. The JS
// regex substitutions became literal-anchor splices over the same templates
// (tpl_splice below), so the output stays byte-identical to the page's.
#include <ui_logic.h>
#include <ui_mobile.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- StrBuf

static void sb_reserve(StrBuf *sb, const size_t extra) {
    if (sb->len + extra + 1 <= sb->cap) return;
    size_t cap = sb->cap ? sb->cap : 256;
    while (sb->len + extra + 1 > cap) cap *= 2;
    char *data = realloc(sb->data, cap);
    if (!data) return; // keep the old buffer; the append is dropped
    sb->data = data;
    sb->cap = cap;
}

void sb_append_n(StrBuf *sb, const char *s, const size_t n) {
    if (!s || !n) return;
    sb_reserve(sb, n);
    if (sb->len + n + 1 > sb->cap) return;
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = 0;
}

void sb_append(StrBuf *sb, const char *s) {
    if (s) sb_append_n(sb, s, strlen(s));
}

void sb_append_esc(StrBuf *sb, const char *s) {
    if (!s) return;
    for (; *s; s++) {
        switch (*s) {
            case '&': sb_append(sb, "&amp;"); break;
            case '<': sb_append(sb, "&lt;"); break;
            case '>': sb_append(sb, "&gt;"); break;
            case '"': sb_append(sb, "&quot;"); break;
            case '\'': sb_append(sb, "&#39;"); break;
            default: sb_append_n(sb, s, 1); break;
        }
    }
}

void sb_appendf(StrBuf *sb, const char *fmt, ...) {
    char stack[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(stack, sizeof(stack), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t) n < sizeof(stack)) {
        sb_append_n(sb, stack, (size_t) n);
        return;
    }
    char *heap = malloc((size_t) n + 1);
    if (!heap) return;
    va_start(ap, fmt);
    vsnprintf(heap, (size_t) n + 1, fmt, ap);
    va_end(ap);
    sb_append_n(sb, heap, (size_t) n);
    free(heap);
}

char *sb_finish(StrBuf *sb) {
    char *out = sb->data;
    if (!out) out = calloc(1, 1);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
    return out;
}

void sb_free(StrBuf *sb) {
    free(sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

// ---------------------------------------------------------------- templating
// Cursor-based splicing over a template: copy up to and including `after`, append
// `replacement`, then skip to `until` (which stays in the output). This replaces the JS
// `tpl.replace(/after[^x]*until/, ...)` idiom with literal anchors. When an anchor is
// missing the rest of the template is copied unchanged (like a non-matching regex).

typedef struct {
    const char *cur;
} Tpl;

static void tpl_splice(StrBuf *out, Tpl *t, const char *after, const char *until, const char *replacement, const int escape) {
    const char *a = strstr(t->cur, after);
    if (!a) return;
    const char *end_after = a + strlen(after);
    const char *u = strstr(end_after, until);
    if (!u) return;
    sb_append_n(out, t->cur, (size_t) (end_after - t->cur));
    if (escape) sb_append_esc(out, replacement);
    else sb_append(out, replacement);
    t->cur = u;
}

static void tpl_finish(StrBuf *out, Tpl *t) {
    sb_append(out, t->cur);
    t->cur += strlen(t->cur);
}

// tpl.split(needle).join(replacement)
static void sb_append_replace_all(StrBuf *out, const char *tpl, const char *needle, const char *replacement) {
    const size_t nlen = strlen(needle);
    const char *cur = tpl;
    for (;;) {
        const char *hit = nlen ? strstr(cur, needle) : NULL;
        if (!hit) {
            sb_append(out, cur);
            return;
        }
        sb_append_n(out, cur, (size_t) (hit - cur));
        sb_append(out, replacement);
        cur = hit + nlen;
    }
}

// ---------------------------------------------------------------- window files

#define UI_WINDOWS_MAX 24
#define UI_SETTINGS_TABS_MAX 8

typedef struct {
    char key[8];
    char header[40];
    int sticky;
    char *html;      // windows[key].file content
    char *card;      // "card" (6), "item" (10), "mapCard" (host)
    char part_name[8];
} UiWindowFile;

static UiWindowFile g_files[UI_WINDOWS_MAX];
static int g_file_count;
static char *g_settings_head;
static char *g_settings_tabs[UI_SETTINGS_TABS_MAX];
static int g_settings_tab_count;
static char g_settings_header[40] = "Game Settings";
static int g_settings_sticky = 1;
static int g_loaded;

static char *windows_dir(void) {
#ifdef __EMSCRIPTEN__
    return concat("/ui/windows/", "");       // --preload-file ui/windows@/ui/windows
#else
    return concat(assets_path(), "../ui/windows/");
#endif
}

static char *load_window_file(const char *dir, const char *file) {
    if (!file || !*file) return NULL;
    char *path = concat(dir, file);
    unsigned char *data = NULL;
    size_t length = 0;
    if (read_file(path, &data, &length) != 0) {
        fprintf(stderr, "[ui] missing window file %s\n", path);
        free(path);
        return NULL;
    }
    free(path);
    // Strip one trailing newline the generator may have left (the JS strings had none).
    while (length && (data[length - 1] == '\n' || data[length - 1] == '\r')) data[--length] = 0;
    return (char *) data;
}

static const char *json_str(const cJSON *obj, const char *key, const char *fallback) {
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(item) ? cJSON_GetStringValue(item) : fallback;
}

int ui_windows_load(void) {
    if (g_loaded) return 1;
    g_loaded = 1;

    char *dir = windows_dir();
    char *index_path = concat(dir, "windows.json");
    unsigned char *data = NULL;
    size_t length = 0;
    if (read_file(index_path, &data, &length) != 0) {
        fprintf(stderr, "[ui] cannot read %s: windows are unavailable\n", index_path);
        free(index_path);
        free(dir);
        return 0;
    }
    free(index_path);

    cJSON *root = cJSON_Parse((const char *) data);
    free(data);
    if (!root) {
        fprintf(stderr, "[ui] windows.json: parse error\n");
        free(dir);
        return 0;
    }

    const cJSON *windows = cJSON_GetObjectItem(root, "windows");
    const cJSON *entry;
    cJSON_ArrayForEach(entry, windows) {
        if (g_file_count == UI_WINDOWS_MAX || !entry->string) continue;
        UiWindowFile *w = &g_files[g_file_count++];
        snprintf(w->key, sizeof(w->key), "%s", entry->string);
        snprintf(w->header, sizeof(w->header), "%s", json_str(entry, "header", ""));
        w->sticky = cJSON_IsTrue(cJSON_GetObjectItem(entry, "sticky"));
        w->html = load_window_file(dir, json_str(entry, "file", NULL));
        static const char *parts[] = {"card", "item", "mapCard"};
        for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
            const char *file = json_str(entry, parts[i], NULL);
            if (!file) continue;
            w->card = load_window_file(dir, file);
            snprintf(w->part_name, sizeof(w->part_name), "%s", parts[i]);
            break;
        }
    }

    const cJSON *settings = cJSON_GetObjectItem(root, "settings");
    if (settings) {
        snprintf(g_settings_header, sizeof(g_settings_header), "%s", json_str(settings, "header", "Game Settings"));
        const cJSON *sticky = cJSON_GetObjectItem(settings, "sticky");
        if (cJSON_IsBool(sticky)) g_settings_sticky = cJSON_IsTrue(sticky);
        g_settings_head = load_window_file(dir, json_str(settings, "head", NULL));
        const cJSON *tab;
        cJSON_ArrayForEach(tab, cJSON_GetObjectItem(settings, "tabs")) {
            if (g_settings_tab_count == UI_SETTINGS_TABS_MAX || !cJSON_IsString(tab)) continue;
            g_settings_tabs[g_settings_tab_count] = load_window_file(dir, cJSON_GetStringValue(tab));
            char *mobile = ui_mobile_settings_html(g_settings_tabs[g_settings_tab_count]); // isMobile: "mobile" rows shown
            if (mobile) {
                free(g_settings_tabs[g_settings_tab_count]);
                g_settings_tabs[g_settings_tab_count] = mobile;
            }
            g_settings_tab_count++;
        }
    }

    cJSON_Delete(root);
    free(dir);
    return 1;
}

static const UiWindowFile *find_file(const char *key) {
    for (int i = 0; i < g_file_count; i++) {
        if (!strcmp(g_files[i].key, key)) return &g_files[i];
    }
    return NULL;
}

const char *ui_windows_html(const char *key) {
    const UiWindowFile *w = find_file(key);
    return w && w->html ? w->html : "";
}

const char *ui_windows_part(const char *key, const char *part) {
    const UiWindowFile *w = find_file(key);
    return w && w->card && !strcmp(w->part_name, part) ? w->card : "";
}

int ui_settings_tab_count(void) { return g_settings_tab_count; }
const char *ui_settings_tab_html(const int tab) {
    return tab >= 0 && tab < g_settings_tab_count && g_settings_tabs[tab] ? g_settings_tabs[tab] : "";
}
const char *ui_settings_head_html(void) { return g_settings_head ? g_settings_head : ""; }

// ---------------------------------------------------------------- Krunker tables

// Krunker's class window does not list this one (premium only).
static int class_card_hidden(const char *name) {
    return name && !strcmp(name, "Deagler");
}

// Krunker's host window map cards: name -> thumb index, author, year, version.
typedef struct {
    const char *name;
    int thumb;
    const char *by, *year, *version;
} HostMapMeta;

static const HostMapMeta HOST_MAPS[] = {
    {"Burg", 0, "", "2018", "v3.2"}, {"Littletown", 1, "", "2018", "v0.9"},
    {"Sandstorm", 2, "CrispyCrust", "2019", "v2.3"}, {"Subzero", 3, "CrispyCrust", "2019", "v0.9"},
    {"Undergrowth", 4, "CrispyCrust", "2019", "v1.2"}, {"Shipyard", 5, "Chwoubaka", "2019", "v1.0"},
    {"Freight", 6, "", "2019", "v2.1"}, {"Lostworld", 7, "DasIstWalter", "2020", "v1.2"},
    {"Citadel", 8, "", "2020", "v2.4"}, {"Oasis", 9, "DasIstWalter", "2020", "v0.2"},
    {"Kanji", 10, "Arbiter", "2020", "v5.2"}, {"Newtown", 11, "Arbiter", "2020", "v0.1"},
    {"Industry", 12, "CrispyCrust", "2020", "v1.0"}, {"Soul Sanctum", 13, "", "2020", ""},
    {"Evacuation", 14, "Creationfier", "2020", "vTest"},
};

static const HostMapMeta *map_meta(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof(HOST_MAPS) / sizeof(HOST_MAPS[0]); i++) {
        if (!strcmp(HOST_MAPS[i].name, name)) return &HOST_MAPS[i];
    }
    return NULL;
}

const char *ui_map_thumb(const char *name) {
    static char thumb[48];
    const HostMapMeta *m = map_meta(name);
    if (!m) return "img/noimg.png";
    snprintf(thumb, sizeof(thumb), "img/maps/map_%d.png", m->thumb);
    return thumb;
}

// ---------------------------------------------------------------- generators

static char *gen_settings(void) {
    const MenuUi *ui = menu_ui();
    StrBuf out = {0};
    // mark the active tab: the n-th `class="settingTab "` becomes `settingTab tabANew`
    const char *needle = "class=\"settingTab \"";
    const size_t nlen = strlen(needle);
    const char *cur = ui_settings_head_html();
    int n = 0;
    for (;;) {
        const char *hit = strstr(cur, needle);
        if (!hit) break;
        sb_append_n(&out, cur, (size_t) (hit - cur));
        sb_append(&out, n++ == ui->settings_tab ? "class=\"settingTab tabANew\"" : "class=\"settingTab \"");
        cur = hit + nlen;
    }
    sb_append(&out, cur);
    sb_append(&out, "<div id=\"settHolder\">");
    sb_append(&out, ui_settings_tab_html(ui->settings_tab));
    sb_append(&out, "</div>");
    return sb_finish(&out);
}

static char *gen_classes(void) {
    const MenuUi *ui = menu_ui();
    const char *card = ui_windows_part("6", "card");
    StrBuf out = {0};
    sb_append(&out, "<div class=\"skinList\">");
    for (int i = 0; i < ui->class_count; i++) {
        const MenuClass *c = &ui->classes[i];
        if (class_card_hidden(c->name)) continue;
        char num[16];
        Tpl t = {card};
        // .replace(/selectClass\(\d+\)/, 'selectClass(' + index + ')')
        snprintf(num, sizeof(num), "%d", c->index);
        tpl_splice(&out, &t, "selectClass(", ")", num, 0);
        // .replace(/onmouseenter="playTick\(\)">[^<]*<div class="classWeap">[^<]*<\/div>/, ...)
        tpl_splice(&out, &t, "onmouseenter=\"playTick()\">", "<div class=\"classWeap\">", c->name ? c->name : "", 1);
        tpl_splice(&out, &t, "<div class=\"classWeap\">", "</div>", c->weapon ? c->weapon : "", 1);
        // .replace(/src="[^"]*"/, ...)
        tpl_splice(&out, &t, "src=\"", "\"", c->icon, 1);
        // .replace(/<div class="classHP">\d+/, ...)
        snprintf(num, sizeof(num), "%d", c->hp);
        tpl_splice(&out, &t, "<div class=\"classHP\">", "<", num, 0);
        tpl_finish(&out, &t);
    }
    sb_append(&out, "</div>");
    return sb_finish(&out);
}

// The Custom Games list is server data in Krunker (the `maps` popular request, rendered by
// the client's genList into .mapListItem cards). KrunkNative has no map browser server, so
// this is the list the v3.1.1 offline server returns to a guest (captured in
// doc/ref/krunker-v3.1.1/windows/win-games.html): no thumbnails (map_image is empty, so
// the card keeps ./img/noimg.png), 0 votes, Krunker's own maps first, then its
// community/custom maps. Fields: map_name, map_id, creatorname, map_votes, mod_url.
typedef struct {
    const char *name;
    const char *id;
    const char *creator;
    int votes;
    const char *mod_url;
} GameListEntry;

static const GameListEntry GAME_LIST[] = {
    {"Soul Sanctum", "2717856513", "Krunker", 0, ""},
    {"Subzero", "1389891507", "Krunker", 0, ""},
    {"Undergrowth", "2699178862", "Krunker", 0, ""},
    {"Lostworld", "2026576977", "Krunker", 0, ""},
    {"Newtown", "4056084657", "Krunker", 0, ""},
    {"Oasis", "2101154094", "Krunker", 0, ""},
    {"Sandstorm", "88334292", "Krunker", 0, ""},
    {"Shipyard", "2613134335", "Krunker", 0, ""},
    {"Freight", "1010451404", "Krunker", 0, ""},
    {"Industry", "312130495", "Krunker", 0, ""},
    {"Kanji", "1336255044", "Krunker", 0, ""},
    {"Littletown", "3836435553", "Krunker", 0, ""},
    {"Burg", "1257833875", "Krunker", 0, ""},
    {"Citadel", "1963634499", "Krunker", 0, ""},
    {"Evacuation", "4241009034", "Krunker", 0, ""},
    {"Zombie_High", "569198284", "IamFiercer", 0, "https://user-assets.krunker.io/md23677/mod.zip"},
    {"Sandstorm", "88334292", "Community", 0, ""},
    {"Shipyard", "2613134335", "Community", 0, ""},
    {"Subzero", "1389891507", "Community", 0, ""},
    {"Undergrowth", "2699178862", "Community", 0, ""},
    {"slide_moonlight", "3690042710", "Community", 0, ""},
    {"Burg", "1257833875", "Community", 0, ""},
    {"Freight", "1010451404", "Community", 0, ""},
    {"Kanji", "1336255044", "Community", 0, ""},
    {"Littletown", "3836435553", "Community", 0, ""},
    {"Lostworld", "2026576977", "Community", 0, ""},
    {"SniperRoyale2", "2800345118", "Community", 0, "https://www.dropbox.com/s/0h3mnvii1nua0h9/Logo.zip?dl=0"},
    {"Test", "2949673445", "Community", 0, ""},
    {"Verb_Arena", "682138864", "Community", 0, "https://www.dropbox.com/s/ejrn99ayvddiq45/Verb%20Mod%20Updated%207April2021.zip?dl=0"},
    {"eterno_jump", "739315237", "Community", 0, ""},
    {"Krunk_Plaza", "537274925", "Community", 0, ""},
    {"Slide_Neon_Stage", "197077862", "R75", 0, ""},
    {"Bhop_VIP", "3459552096", "eskrr", 0, ""},
    {"ClashRoyale", "2789783172", "Community", 0, ""},
    {"ESP_Parkour", "2793053018", "ImCriistian", 0, ""},
    {"FortniteTiltedT", "563473844", "Community", 0, ""},
};

// The other tabs as the v3.1.1 offline server answers them for a guest (captured, indices
// into GAME_LIST): Featured (no Load More), New, My Games (no Load More); Favorites is
// empty ("No Games found!") and Search shows the search box (its request never answers).
static const unsigned char GAMES_FEATURED[] = {9, 1, 3, 14, 2, 10, 7, 8, 6, 5, 11, 4, 0, 13, 12};
static const unsigned char GAMES_NEW[] = {1, 2, 4, 5, 6, 7, 0, 9, 10, 11, 3, 13, 14, 8, 12, 17, 18, 19, 20, 15, 22, 23, 24, 25, 16, 27, 28, 29, 21, 31, 26, 33, 34, 35, 30, 32};
static const unsigned char GAMES_MINE[] = {12, 13, 14, 8, 9, 10, 11, 3, 4, 5, 6, 7, 0, 1, 2, 32, 33, 34, 35, 30, 31, 26, 27, 28, 29, 21, 22, 23, 24, 25, 16, 17, 18, 19, 20, 15};
static int g_games_tab; // windows[9].tabIndex

void ui_games_switch_tab(const int tab) {
    g_games_tab = tab >= 0 && tab <= 5 ? tab : 0;
    menu_rerender_if_open(10);
}

// windows[9].gen() for the map tab: header markup + one .mapListItem per entry (genList).
static char *gen_games(void) {
    const char *item = ui_windows_part("10", "item");
    StrBuf out = {0};
    // the header template has Popular active: move tabANew to the current tab
    const char *needle = "class=\"menuTabNew tabANew\"";
    const char *head = ui_windows_html("10");
    const char *hit = strstr(head, needle);
    if (hit && g_games_tab) {
        sb_append_n(&out, head, (size_t) (hit - head));
        sb_append(&out, "class=\"menuTabNew\"");
        const char *rest = hit + strlen(needle);
        char tab[80];
        snprintf(tab, sizeof(tab), "class=\"menuTabNew\" onclick=\"windows[9].switchTab(%d)\"", g_games_tab);
        const char *t = strstr(rest, tab);
        if (t) {
            sb_append_n(&out, rest, (size_t) (t - rest));
            sb_append(&out, "class=\"menuTabNew tabANew\"");
            sb_append(&out, t + strlen("class=\"menuTabNew\""));
        } else {
            sb_append(&out, rest);
        }
    } else {
        sb_append(&out, head);
    }
    sb_append(&out, "<div style=\"color:rgba(0,0,0,0.3)\" id=\"mapList\">");
    if (g_games_tab == 5) {
        sb_append(&out, "<input type=\"text\" id=\"mpSrch\" class=\"smlInput\" placeholder=\"Game Name\" onkeydown=\"if(window.event.keyCode==13)windows[9].searchMaps()\"><div class=\"searchBtn\" onclick=\"windows[9].searchMaps()\">Search</div><div style=\"color:rgba(0,0,0,0.5);margin-top:10px\" id=\"searchResultsMA\"></div></div>");
        return sb_finish(&out);
    }
    const unsigned char *order = NULL;
    size_t count = sizeof(GAME_LIST) / sizeof(GAME_LIST[0]);
    int paged = 1;
    switch (g_games_tab) {
        case 1: order = GAMES_FEATURED; count = sizeof(GAMES_FEATURED); paged = 0; break;
        case 2: order = GAMES_NEW; count = sizeof(GAMES_NEW); break;
        case 3: count = 0; break;
        case 4: order = GAMES_MINE; count = sizeof(GAMES_MINE); paged = 0; break;
        default: break;
    }
    for (size_t i = 0; i < count; i++) {
        const GameListEntry *e = &GAME_LIST[order ? order[i] : i];
        char url[160], host_args[320], fav[48], votes[16];
        snprintf(url, sizeof(url), "/social.html?p=map&amp;q=%s", e->name);
        snprintf(host_args, sizeof(host_args), "%s&quot;,&quot;%s&quot;,&quot;%s&quot;,&quot;%s", e->name, e->mod_url, e->id, e->creator);
        snprintf(fav, sizeof(fav), "0, this, %s", e->id);
        snprintf(votes, sizeof(votes), "%d", e->votes);

        Tpl t = {item};
        tpl_splice(&out, &t, "openURL(&quot;", "&quot;);", url, 0);             // mapInfoB
        tpl_splice(&out, &t, "playMap(&quot;", "&quot;);", e->name, 0);         // searchIcon
        tpl_splice(&out, &t, "favoriteM(", ");", fav, 0);                       // creationAction
        tpl_splice(&out, &t, "selectHostMap(&quot;", "&quot;)", host_args, 0);  // thumbnail
        tpl_splice(&out, &t, "selectHostMap(&quot;", "&quot;)", host_args, 0);  // anchor
        tpl_splice(&out, &t, "&quot;)\">", "</a>", e->name, 0);                 // anchor text
        tpl_splice(&out, &t, "profile&amp;q=", "\"", e->creator, 0);            // creator link
        tpl_splice(&out, &t, "\">", "</a>", e->creator, 0);                     // creator text
        tpl_splice(&out, &t, "flex-grow:1\">", "<", votes, 0);                  // votes
        tpl_finish(&out, &t);
    }
    if (!count) sb_append(&out, "No Games found!");
    // Popular / New are paged (no sendID/noPages): Krunker appends its Load More button
    else if (paged) sb_append(&out, "<div class=\"loadMoreD\" onclick=\"windows[9].loadMore(this)\">Load More</div>");
    sb_append(&out, "</div>");
    return sb_finish(&out);
}

// Real genHost (windows[7]): every default map (Krunker's `bg` list, = g_maps) except
// Soul Sanctum, unchecked, `gameMap<index>` ids and map_<index> thumbnails; the boxes are
// read back by createPrivateRoom (ui_host_checked_map). No community game selected (eI).
static char *gen_host(void) {
    load_default_maps();
    StrBuf cards = {0};
    for (size_t i = 0; i < sizeof(g_maps) / sizeof(g_maps[0]); i++) {
        const cJSON *name_item = g_maps[i] ? cJSON_GetObjectItem(g_maps[i], "name") : NULL;
        if (!cJSON_IsString(name_item)) continue;
        const char *name = cJSON_GetStringValue(name_item);
        if (!strcmp(name, "Soul Sanctum")) continue;
        const HostMapMeta *meta = map_meta(name);
        sb_append(&cards, "<label class=\"hostMap\"><div class=\"hostMapName blackShad\">");
        // aU.capFirst(name)
        char first[2] = {(char) toupper((unsigned char) name[0]), 0};
        sb_append_esc(&cards, first);
        if (name[0]) sb_append_esc(&cards, name + 1);
        sb_append(&cards, "</div>");
        if (meta && meta->by[0]) {
            sb_append(&cards, "<div class=\"hostMapBy blackShad\">by ");
            sb_append_esc(&cards, meta->by);
            sb_append(&cards, "</div>");
        }
        if (meta && meta->year[0]) sb_appendf(&cards, "<div class=\"hostMapYear blackShad\">%s</div>", meta->year);
        if (meta && meta->version[0]) sb_appendf(&cards, "<div class=\"hostMapVersion blackShad\">%s</div>", meta->version);
        sb_appendf(&cards, "<div class=\"mapInfoT\" style=\"top:54px\">?</div><input id=\"gameMap%zu\" type=\"checkbox\">", i);
        sb_appendf(&cards, "<img loading=\"lazy\" draggable=\"false\" class=\"hostMapImg\" src=\"img/maps/map_%zu.png\"></label>", i);
    }
    // "Select Modes": every mode without preventCustomGames (shared mode table), capFirst
    // names, `gameMode<id>` ids, Free for All checked (no previous custom room).
    StrBuf modes = {0};
    int mode_total = 0;
    const GameModeConfig *list = mode_list(&mode_total);
    sb_append(&modes, "<div style=\"margin-top:5px\">");
    for (int i = 0; list && i < mode_total; i++) {
        if (!mode_hostable(i)) continue;
        const char *mname = list[i].name ? list[i].name : "";
        char first[2] = {(char) toupper((unsigned char) mname[0]), 0};
        sb_append(&modes, "<label class=\"hostOpt\" onmouseenter=\"playTick()\"><span class=\"optName\">");
        sb_append_esc(&modes, first);
        if (mname[0]) sb_append_esc(&modes, mname + 1);
        sb_appendf(&modes, "</span><input id=\"gameMode%d\" type=\"checkbox\"%s><div class=\"optCheck\"></div></label>", i, i == 0 ? " checked=\"\"" : "");
    }
    sb_append(&modes, "</div>");
    char *tpl = NULL;
    {
        StrBuf with_maps = {0};
        sb_append_replace_all(&with_maps, ui_windows_html("host"), "<!--MAPS-->", cards.data ? cards.data : "");
        tpl = sb_finish(&with_maps);
    }
    sb_free(&cards);
    StrBuf out = {0};
    // splice the generated list over the captured one (between the Toggle header and the
    // next section header)
    const char *anchor = "windows[7].toggle(&quot;gameMode&quot;)\">Toggle</div></div>";
    const char *a = strstr(tpl, anchor);
    const char *b = a ? strstr(a, "<div class=\"settName b\" style=\"margin-top:20px\">") : NULL;
    if (a && b) {
        a += strlen(anchor);
        sb_append_n(&out, tpl, (size_t) (a - tpl));
        sb_append(&out, modes.data ? modes.data : "");
        sb_append(&out, b);
    } else {
        sb_append(&out, tpl);
    }
    sb_free(&modes);
    free(tpl);
    return sb_finish(&out);
}

// createPrivateRoom's mode read-back: the first checked hostable #gameMode<id> (Krunker
// rotates through all checked modes; the offline server plays one). -1 = none checked.
int ui_host_checked_mode(void) {
    int total = 0;
    mode_list(&total);
    for (int i = 0; i < total; i++) {
        char id[24];
        snprintf(id, sizeof(id), "gameMode%d", i);
        HtmlElem box = mode_hostable(i) ? html_dom_by_id(id) : 0;
        if (box && html_dom_get_checked(box)) return i;
    }
    return -1;
}

// createPrivateRoom's map read-back: the first checked #gameMap<i> box of the host window
// (Krunker sends every checked map; KrunkNative's local server plays one). Returns the
// map's name, or NULL when none is checked.
const char *ui_host_checked_map(void) {
    for (size_t i = 0; i < sizeof(g_maps) / sizeof(g_maps[0]); i++) {
        char id[24];
        snprintf(id, sizeof(id), "gameMap%zu", i);
        HtmlElem box = html_dom_by_id(id);
        if (!box || !html_dom_get_checked(box) || !g_maps[i]) continue;
        const cJSON *name = cJSON_GetObjectItem(g_maps[i], "name");
        if (cJSON_IsString(name)) return cJSON_GetStringValue(name);
    }
    return NULL;
}

static char *gen_servers(void) {
    StrBuf out = {0};
    StrBuf region = {0};
    sb_append_esc(&region, MENU_REGION_NAME);
    sb_append_replace_all(&out, ui_windows_html("2"), "New Jersey", region.data ? region.data : "");
    sb_free(&region);
    return sb_finish(&out);
}

static char *gen_static(const char *key) {
    StrBuf out = {0};
    sb_append(&out, ui_windows_html(key));
    return sb_finish(&out);
}

typedef struct {
    int id;
    const char *key;
    const char *header;
    int sticky;
    char *(*gen)(void);
} WindowDef;

static char *gen_4(void) { return gen_static("4"); }
static char *gen_5(void) { return gen_static("5"); }
static char *gen_7(void) { return ui_controls_window_html(); } // live binds (ui_controls.c); 7.html is the default-binds capture
static char *gen_14(void) { return gen_static("14"); }
static char *gen_25(void) { return gen_static("25"); }
static char *gen_27(void) { return gen_static("27"); }
static char *gen_33(void) { return gen_static("33"); }
static char *gen_join(void) { return gen_static("join"); }
static char *gen_9(void) { return gen_static("9"); }   // Select Spray (guest: the default sprays)
static char *gen_21(void) { return gen_static("21"); } // Advertise (guest: "Please Login to Submit a Bid")
static char *gen_29(void) { return gen_static("29"); } // Support
// Change Loadout and the windows it opens, Changelog: ui_loadout.c
static char *gen_loadout(void) { return ui_loadout_gen(); }
#define LOADOUT_GEN(n) static char *gen_##n(void) { return ui_loadout_generate(n, NULL); }
LOADOUT_GEN(15) LOADOUT_GEN(16) LOADOUT_GEN(17) LOADOUT_GEN(20) LOADOUT_GEN(26) LOADOUT_GEN(31)
LOADOUT_GEN(32) LOADOUT_GEN(34) LOADOUT_GEN(35) LOADOUT_GEN(38) LOADOUT_GEN(39)
#undef LOADOUT_GEN

static const WindowDef WINDOWS[] = {
    {1, "1", "Game Settings", 1, gen_settings},
    {2, "2", "Servers", 1, gen_servers},
    {3, "3", "Change Loadout", 0, gen_loadout},
    {4, "4", "Mod Manager", 0, gen_4},
    {5, "5", "Account", 0, gen_5},
    {6, "6", "Select Class", 0, gen_classes},
    {7, "7", "Change Controls", 0, gen_7},
    {10, "10", "Custom Games", 0, gen_games},
    {14, "14", "Store", 0, gen_14},
    {25, "25", "Client", 0, gen_25},
    {27, "27", "Ranked", 0, gen_27},
    {33, "33", "Challenges", 0, gen_33},
    {9, "9", "Select Spray", 0, gen_9},
    {15, "15", "Select Skin", 0, gen_15},
    {16, "16", "Select Hat", 0, gen_16},
    {17, "17", "Select Back", 0, gen_17},
    {20, "20", "Select Secondary", 0, gen_20},
    {21, "21", "Advertise", 0, gen_21},
    {26, "26", "Select Melee", 0, gen_26},
    {29, "29", "Support", 0, gen_29},
    {31, "31", "Select Attachment", 0, gen_31},
    {32, "32", "Select Reticle", 0, gen_32},
    {34, "34", "Select Dye", 0, gen_34},
    {35, "35", "Changelog", 0, gen_35},
    {38, "38", "Select Waist", 0, gen_38},
    {39, "39", "Select Face", 0, gen_39},
    {MENU_WINDOW_HOST, "host", "Host Custom Game", 0, gen_host},
    {MENU_WINDOW_JOIN, "join", "Join", 0, gen_join},
};

static const WindowDef *find_window(const int id) {
    for (size_t i = 0; i < sizeof(WINDOWS) / sizeof(WINDOWS[0]); i++) {
        if (WINDOWS[i].id == id) return &WINDOWS[i];
    }
    return NULL;
}

const char *ui_window_key(const int id) {
    const WindowDef *w = find_window(id);
    return w ? w->key : NULL;
}

char *ui_window_generate(const int id, const char **header, int *sticky) {
    const WindowDef *w = find_window(id);
    if (!w) return NULL;
    ui_windows_load();

    // windows.json overrides the header/sticky defaults, like the JS did with WIN.windows[k].
    const char *hdr = w->header;
    int stk = w->sticky;
    if (id == 1) {
        if (g_settings_header[0]) hdr = g_settings_header;
        if (g_settings_sticky) stk = 1;
    } else {
        const UiWindowFile *f = find_file(w->key);
        if (f && f->header[0]) hdr = f->header;
        if (f && f->sticky) stk = 1;
    }
    if (header) *header = hdr;
    if (sticky) *sticky = stk;
    return w->gen();
}

void ui_window_post(const int id) {
    if (id == 1) {
        ui_settings_apply_values(html_dom_by_id("menuWindow"));
    } else if (id == 5) {
        const MenuUi *ui = menu_ui();
        HtmlElem input = html_dom_by_id("accName");
        if (input) html_dom_set_value(input, ui->name_set ? ui->client->menu_name : "");
    }
}
