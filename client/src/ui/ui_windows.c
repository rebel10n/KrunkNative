// Menu window markup: loads ui/windows/windows.json + the captured window files and
// generates what krunknative.js's WINDOWS[id].gen() wrote into #menuWindow. The JS
// regex substitutions became literal-anchor splices over the same templates
// (tpl_splice below), so the output stays byte-identical to the page's.
#include <ui_logic.h>
#include <backend_ui.h>
#include <map_catalog.h>
#include <ui_mobile.h>
#include <client_settings.h>
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

// mapThumb(): the official map's public/img/maps card, else img/noimg.png (map_image "").
const char *ui_map_thumb(const char *name) {
    static char thumb[48];
    const int index = name ? map_catalog_find(name) : -1;
    const char *src = index >= 0 && !strcmp(map_catalog_at((size_t) index)->name, name)
                          ? map_catalog_thumb(map_catalog_at((size_t) index), thumb, sizeof(thumb)) : NULL;
    return src ? src : "img/noimg.png";
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
    // the desktop client's "Client" tab after Krunker's six (ui_client_settings.c)
    char *head = ui_client_settings_head(out.data ? out.data : "", ui->settings_tab);
    sb_free(&out);
    sb_append(&out, head);
    free(head);
    sb_append(&out, "<div id=\"settHolder\">");
    // acc && tabIndex == 0: the account's settings backup (social socket uploadS / getS)
    if (account_get() && ui->settings_tab == 0) sb_append(&out, "<div class=\"settName\" style=\"margin-top:15px\">\n                Backup Settings\n                <div class=\"settingsBtn\" style=\"width: auto;\" onclick=\"uploadSettings()\">Upload</div>\n                <div class=\"settingsBtn\" style=\"width: auto;\" onclick=\"fetchSettings()\">Download</div>\n            </div>");
    if (ui->settings_tab >= 0 && ui->settings_tab == ui_client_settings_tab()) {
        char *client = ui_client_settings_tab_html(0);
        sb_append(&out, client);
        free(client);
    } else {
        sb_append(&out, ui_settings_tab_html(ui->settings_tab));
    }
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
// the client's genList into .mapListItem cards): the v3.1.1 offline server lists its
// maps/ and custom_maps/ (publicRoutes.ts loadMaps), i.e. the map catalogue
// (shared/src/map_catalog.c), in the orders it answered a guest (captured in
// doc/ref/krunker-v3.1.1/windows/win-games.html): no thumbnails (map_image is empty, so
// the card keeps ./img/noimg.png), 0 votes. Fields: map_name, map_id, creatorname,
// map_votes, mod_url. Favorites is empty ("No Games found!") and Search shows the search
// box (its request never answers).
static int g_games_tab; // windows[9].tabIndex

void ui_games_switch_tab(const int tab) {
    g_games_tab = tab >= 0 && tab <= 5 ? tab : 0;
    menu_rerender_if_open(10);
}

// One .mapListItem of the captured template (getCardsList) for a map row.
static void games_item(StrBuf *out, const char *item, const char *name, const char *mod_url, const char *id, const char *creator, const int vote_count) {
    char url[160], host_args[320], fav[48], votes[16];
    snprintf(url, sizeof(url), "/social.html?p=map&amp;q=%s", name);
    snprintf(host_args, sizeof(host_args), "%s&quot;,&quot;%s&quot;,&quot;%s&quot;,&quot;%s", name, mod_url, id, creator);
    snprintf(fav, sizeof(fav), "0, this, %s", id);
    snprintf(votes, sizeof(votes), "%d", vote_count);

    Tpl t = {item};
    tpl_splice(out, &t, "openURL(&quot;", "&quot;);", url, 0);             // mapInfoB
    tpl_splice(out, &t, "playMap(&quot;", "&quot;);", name, 0);            // searchIcon
    tpl_splice(out, &t, "favoriteM(", ");", fav, 0);                       // creationAction
    tpl_splice(out, &t, "selectHostMap(&quot;", "&quot;)", host_args, 0);  // thumbnail
    tpl_splice(out, &t, "selectHostMap(&quot;", "&quot;)", host_args, 0);  // anchor
    tpl_splice(out, &t, "&quot;)\">", "</a>", name, 0);                   // anchor text
    tpl_splice(out, &t, "profile&amp;q=", "\"", creator, 0);              // creator link
    tpl_splice(out, &t, "\">", "</a>", creator, 0);                       // creator text
    tpl_splice(out, &t, "flex-grow:1\">", "<", votes, 0);                 // votes
    tpl_finish(out, &t);
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
    // Featured / My Games come back whole (no Load More); Popular / New are paged
    const int paged = g_games_tab == 0 || g_games_tab == 2;
    // brain (servers_ui.c): the rows of GET /api/maps?index=<tab> (catalogue + published maps)
    const cJSON *brain_rows = NULL;
    const int brain = servers_games_tab_rows(g_games_tab, &brain_rows);
    if (brain == 0) {
        sb_append(&out, "<div class=\"loadMoreW\">Loading...</div></div>"); // API_CLIENT.listMaps pending
        return sb_finish(&out);
    }
    if (brain > 0) {
        const cJSON *r;
        size_t shown = 0;
        cJSON_ArrayForEach(r, brain_rows) {
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(r, "map_name"));
            const char *creator = cJSON_GetStringValue(cJSON_GetObjectItem(r, "creatorname"));
            const char *mod_url = cJSON_GetStringValue(cJSON_GetObjectItem(r, "mod_url"));
            const cJSON *map_id = cJSON_GetObjectItem(r, "map_id");
            const cJSON *votes = cJSON_GetObjectItem(r, "map_votes");
            if (!name) continue;
            char id[24];
            snprintf(id, sizeof(id), "%.0f", cJSON_IsNumber(map_id) ? map_id->valuedouble : 0.0);
            games_item(&out, item, name, mod_url ? mod_url : "", id, creator ? creator : "Krunker", cJSON_IsNumber(votes) ? votes->valueint : 0);
            shown++;
        }
        if (!shown) sb_append(&out, "No Games found!");
        else if (paged && servers_games_tab_more(g_games_tab)) sb_append(&out, "<div class=\"loadMoreD\" onclick=\"windows[9].loadMore(this)\">Load More</div>");
        sb_append(&out, "</div>");
        return sb_finish(&out);
    }
    int order[64];
    const size_t count = map_catalog_games_tab(g_games_tab, order, sizeof(order) / sizeof(order[0]));
    for (size_t i = 0; i < count; i++) {
        const MapCatalogEntry *e = map_catalog_at((size_t) order[i]);
        char id[16];
        snprintf(id, sizeof(id), "%u", (unsigned) map_catalog_map_id(e));
        games_item(&out, item, e->name, e->mod_url, id, e->creator, 0);
    }
    if (!count) sb_append(&out, "No Games found!");
    // Popular / New are paged (no sendID/noPages): Krunker appends its Load More button
    else if (paged) sb_append(&out, "<div class=\"loadMoreD\" onclick=\"windows[9].loadMore(this)\">Load More</div>");
    sb_append(&out, "</div>");
    return sb_finish(&out);
}

// Real genHost (windows[7]): every bundled map (Krunker's `bg` list = the official
// catalogue maps) except Soul Sanctum, unchecked, `gameMap<mapIndex>` ids and
// map_<mapIndex> thumbnails; the boxes are read back by createPrivateRoom
// (ui_host_checked_map_index). No community game selected (eI).
static char *gen_host(void) {
    StrBuf cards = {0};
    for (size_t i = 0; i < map_catalog_count(); i++) {
        const MapCatalogEntry *e = map_catalog_at(i);
        if (!e->hostable) continue;
        const char *name = e->name;
        char thumb[48];
        sb_append(&cards, "<label class=\"hostMap\"><div class=\"hostMapName blackShad\">");
        // aU.capFirst(name)
        char first[2] = {(char) toupper((unsigned char) name[0]), 0};
        sb_append_esc(&cards, first);
        if (name[0]) sb_append_esc(&cards, name + 1);
        sb_append(&cards, "</div>");
        if (e->host_by[0]) {
            sb_append(&cards, "<div class=\"hostMapBy blackShad\">by ");
            sb_append_esc(&cards, e->host_by);
            sb_append(&cards, "</div>");
        }
        if (e->host_year[0]) sb_appendf(&cards, "<div class=\"hostMapYear blackShad\">%s</div>", e->host_year);
        if (e->host_version[0]) sb_appendf(&cards, "<div class=\"hostMapVersion blackShad\">%s</div>", e->host_version);
        sb_appendf(&cards, "<div class=\"mapInfoT\" style=\"top:54px\">?</div><input id=\"gameMap%d\" type=\"checkbox\">", e->index);
        sb_appendf(&cards, "<img loading=\"lazy\" draggable=\"false\" class=\"hostMapImg\" src=\"%s\"></label>",
                   map_catalog_thumb(e, thumb, sizeof(thumb)) ? thumb : "img/noimg.png");
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

// createPrivateRoom's map read-back: the first checked #gameMap<mapIndex> box of the host
// window (Krunker sends every checked map; KrunkNative's local server plays one). Returns
// its map catalogue index, -1 when none is checked.
int ui_host_checked_map_index(void) {
    for (size_t i = 0; i < map_catalog_count(); i++) {
        const MapCatalogEntry *e = map_catalog_at(i);
        if (!e->hostable) continue;
        char id[24];
        snprintf(id, sizeof(id), "gameMap%d", e->index);
        HtmlElem box = html_dom_by_id(id);
        if (box && html_dom_get_checked(box)) return (int) i;
    }
    return -1;
}

// The same as a map name, NULL when none is checked.
const char *ui_host_checked_map(void) {
    const int index = ui_host_checked_map_index();
    return index >= 0 ? map_catalog_at((size_t) index)->name : NULL;
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
static char *gen_9(void) { // Select Spray: owned sprays signed in (ui_loadout.c), else the default sprays
    char *owned = ui_loadout_generate(9, NULL);
    return owned ? owned : gen_static("9");
}
static char *gen_21(void) { return gen_static("21"); } // Advertise (guest: "Please Login to Submit a Bid")
static char *gen_29(void) { return gen_static("29"); } // Support
static char *gen_30(void) { return strdup(""); }       // Krunker Mail: signed in only (account_ui.c)
static char *gen_13(void) { return clan_window_gen(); } // Clans (windows[12], clan_ui.c): guest LOGIN REQUIRED
// windows[35] Premium ("Things you get with Premium:"), opened by the Store Premium tab's link.
static char *gen_36(void) {
    static const char *const perks[] = {
        "Golden Badge on Profile",
        "Custom Profile Pictures",
        "Custom Display Name",
        "Monthly giveaway entry: Chance to win 50,000 KR",
        "Custom Skin Color",
        "Custom Hair Color",
        "Custom Chat Name Color",
        "Custom Commands: /god /noclip /kill /smite",
        "Can host 16 player lobbies",
        "Can enter Limited Premium Areas on Custom Maps",
        "Increased Object Limit in Customs",
        "Increased Max KR per Round to 40",
        "Increased Market Sale Limit to 40",
        "Bigger Thumbnails for Maps",
        "Increased Trade Limit to 10 items",
        "Increased Asset File Size",
        "Increased Asset Upload Limit",
    };
    StrBuf sb = {0};
    sb_append(&sb, "<div style='font-size: 22px;margin-bottom:10px'>Things you get with Premium:</div><div style='background-color:#333;padding:15px;border-radius:6px;border:5px solid var(--yellow)'>");
    for (size_t i = 0; i < sizeof(perks) / sizeof(perks[0]); i++) sb_appendf(&sb, "<div class='settNameSmall premText'><i class='material-icons'>grade</i> %s</div>", perks[i]);
    sb_append(&sb, "</div>");
    return sb_finish(&sb);
}
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
    {13, "13", "Clans", 0, gen_13},
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
    {30, "30", "Krunker Mail", 0, gen_30},
    {31, "31", "Select Attachment", 0, gen_31},
    {32, "32", "Select Reticle", 0, gen_32},
    {34, "34", "Select Dye", 0, gen_34},
    {35, "35", "Changelog", 0, gen_35},
    {36, "36", "Premium", 0, gen_36},
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
    // brain backend (backend_ui.h): the signed-in Account window and the live Servers
    // list replace the guest captures; NULL keeps them (no brain, guest)
    char *live = account_window_gen(id, header);
    if (!live) live = servers_window_gen(id, header);
    if (live) return live;
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
    account_window_post(id); // brain: the saved krunker_username instead of the nickname
}
