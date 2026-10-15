// Seek preferences: Settings > General > Quickplay (client/include/seek_prefs.h,
// doc/v10/matchmaker-token.md "Seek preferences"). v10.0.2 reads `seek_modes` / `seek_maps`
// (getSavedVal, comma lists of indices; v10.js:222011-222016) into the seek's dataQuery
// dsmo / dsma (v10.js:222051-222055) and has no settings row for them; the rows here are
// KrunkNative's, in v10's settings markup (setHed / setBodH / settName + inputGrey2 select).
#include <seek_prefs.h>
#include <krunker_net.h> // krunker_net_in_client (the Server Type texts)
#include <ui_logic.h>
#include <map_catalog.h>
#include <modes.h>
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define SEEK_MODES_KEY "seek_modes"
#define SEEK_MAPS_KEY "seek_maps"
#define SEEK_SERVERS_KEY "knSeekServers"

// The first index of a saved list ("3", "3,5", 3, [3]); -1 = none / Any.
static int saved_first_index(const char *key) {
    const cJSON *v = ui_setting_saved(key);
    if (cJSON_IsArray(v)) v = cJSON_GetArrayItem(v, 0);
    if (cJSON_IsNumber(v)) return v->valuedouble >= 0 ? (int) v->valuedouble : -1;
    const char *s = cJSON_GetStringValue(v);
    if (!s || !(s[0] >= '0' && s[0] <= '9')) return -1;
    return atoi(s);
}

int seek_prefs_mode(void) {
    const int id = saved_first_index(SEEK_MODES_KEY);
    return id >= 0 && mode_config(id) ? id : -1;
}

int seek_prefs_map(void) {
    const int index = saved_first_index(SEEK_MAPS_KEY);
    return index >= 0 && map_catalog_at((size_t) index) ? index : -1;
}

const char *seek_prefs_map_name(void) {
    const int index = seek_prefs_map();
    return index >= 0 ? map_catalog_at((size_t) index)->name : NULL;
}

SeekServers seek_prefs_servers(void) {
#ifdef __EMSCRIPTEN__
    return SEEK_SERVERS_KN; // platform behaviour: the web client only lists and seeks our servers
#else
    const char *s = cJSON_GetStringValue(ui_setting_saved(SEEK_SERVERS_KEY));
    if (s && !strcmp(s, "official")) return SEEK_SERVERS_OFFICIAL;
    if (s && !strcmp(s, "both")) return SEEK_SERVERS_BOTH;
    return SEEK_SERVERS_KN;
#endif
}

void seek_prefs_data_query(char *out, const size_t n) {
    if (!out || !n) return;
    out[0] = 0;
    const int mode = seek_prefs_mode(), map = seek_prefs_map();
    if (mode < 0 && map < 0) return;
    int len = snprintf(out, n, "{");
    if (mode >= 0) len += snprintf(out + len, n - (size_t) len, "\"dsmo\":[%d]", mode);
    if (map >= 0 && (size_t) len < n) len += snprintf(out + len, n - (size_t) len, "%s\"dsma\":[%d]", mode >= 0 ? "," : "", map);
    if ((size_t) len < n) snprintf(out + len, n - (size_t) len, "}");
}

int seek_prefs_match(const int mode, const char *map_name) {
    const int want_mode = seek_prefs_mode();
    const char *want_map = seek_prefs_map_name();
    if (want_mode >= 0 && mode != want_mode) return 0;
    if (want_map && (!map_name || strcasecmp(map_name, want_map) != 0)) return 0;
    return 1;
}

void seek_prefs_describe(char *out, const size_t n) {
    const int mode = seek_prefs_mode();
    const GameModeConfig *m = mode >= 0 ? mode_config(mode) : NULL;
    const char *map = seek_prefs_map_name();
    i18n_trf(out, n, "kn.quickplay.describe", "{0} on {1}", 2,
             I18N_ARGV(m && m->name ? m->name : I18N("kn.quickplay.anyMode", "any mode"), map ? map : I18N("kn.quickplay.anyMap", "any map")));
}

static void option(StrBuf *sb, const char *value, const char *label, const int selected) {
    sb_append(sb, "<option value=\"");
    sb_append_esc(sb, value);
    sb_append(sb, selected ? "\" selected=\"\">" : "\">");
    sb_append_esc(sb, label);
    sb_append(sb, "</option>");
}

#ifndef __EMSCRIPTEN__
// Server Type (native only: the web client never lists krunker.io lobbies): what Quick Match and
// Find New Game (F4) seek into; the server browser lists both kinds whatever it says. Official
// lobbies are joined in this client (krunker_net.h) unless the browser opt-out is set.
static void server_type_html(StrBuf *sb) {
    const SeekServers servers = seek_prefs_servers();
    const int here = krunker_net_in_client();
    sb_append(sb, "<div class=\"settName\" title=\"");
    sb_append_esc(sb, here ? I18N("kn.quickplay.serverType.infoJoin", "What Quick Match and Find New Game (F4) look for. The server browser lists both; "
                                                                       "krunker.io servers are joined in this client")
                           : I18N("kn.quickplay.serverType.infoBrowser", "What Quick Match and Find New Game (F4) look for. The server browser lists both; "
                                                                          "krunker.io servers open in your browser"));
    sb_append(sb, "\">");
    sb_append_esc(sb, I18N("kn.quickplay.serverType", "Server Type"));
    sb_append(sb, " <select id=\"knSeekServers\" onchange=\"window.setSetting(&quot;" SEEK_SERVERS_KEY "&quot;, this.value)\" class=\"inputGrey2\">");
    option(sb, "kn", "KrunkNative", servers == SEEK_SERVERS_KN);
    option(sb, "official", I18N("kn.quickplay.serverType.official", "krunker.io servers"), servers == SEEK_SERVERS_OFFICIAL);
    option(sb, "both", I18N("kn.quickplay.serverType.both", "Both"), servers == SEEK_SERVERS_BOTH);
    sb_append(sb, "</select></div><div class=\"settName\" style=\"color:rgba(255,255,255,0.5);font-size:15px\">");
    sb_append_esc(sb, here ? I18N("kn.quickplay.serverType.noteJoin", "Quick Match and F4 seek into these. krunker.io servers are joined in this client with your "
                                                                       "krunker.io account (krunker.io may ask for a security check first). Both prefers a KrunkNative server when one matches.")
                           : I18N("kn.quickplay.serverType.noteBrowser", "Quick Match and F4 seek into these. krunker.io servers open in your browser. "
                                                                          "Both prefers a KrunkNative server when one matches."));
    sb_append(sb, "</div>");
}
#endif

static void section_html(StrBuf *sb) {
    char value[16];
    sb_append(sb, "<div class=\"setHed\" id=\"setHed_quickplay\" onclick=\"window.windows[0].collapseFolder(this)\">"
                  "<span class=\"material-icons plusOrMinus\">keyboard_arrow_down</span> ");
    sb_append_esc(sb, I18N("kn.quickplay.header", "Quickplay"));
    sb_append(sb, "</div><div class=\"setBodH\" id=\"setBod_quickplay\">");
    // Seek Mode
    const int mode = seek_prefs_mode();
    sb_append(sb, "<div class=\"settName\" title=\"");
    sb_append_esc(sb, I18N("kn.quickplay.seekMode.info", "Quick Match, Find New Game (F4) and the server browser look for this mode"));
    sb_append(sb, "\">");
    sb_append_esc(sb, I18N("kn.quickplay.seekMode", "Seek Mode"));
    sb_append(sb, " <select id=\"knSeekMode\" onchange=\"window.setSetting(&quot;" SEEK_MODES_KEY "&quot;, this.value)\" class=\"inputGrey2\">");
    option(sb, "", I18N("kn.quickplay.any", "Any"), mode < 0);
    int count = 0;
    const GameModeConfig *modes = mode_list(&count);
    for (int i = 0; modes && i < count; i++) {
        if (!modes[i].name || !modes[i].alias) continue;
        snprintf(value, sizeof(value), "%d", i);
        option(sb, value, modes[i].name, i == mode);
    }
    sb_append(sb, "</select></div>");
    // Seek Map: the official maps (catalogue 0..31 = Krunker's mapIndex)
    const int map = seek_prefs_map();
    sb_append(sb, "<div class=\"settName\" title=\"");
    sb_append_esc(sb, I18N("kn.quickplay.seekMap.info", "Quick Match, Find New Game (F4) and the server browser look for this map"));
    sb_append(sb, "\">");
    sb_append_esc(sb, I18N("kn.quickplay.seekMap", "Seek Map"));
    sb_append(sb, " <select id=\"knSeekMap\" onchange=\"window.setSetting(&quot;" SEEK_MAPS_KEY "&quot;, this.value)\" class=\"inputGrey2\">");
    option(sb, "", I18N("kn.quickplay.any", "Any"), map < 0);
    for (size_t i = 0; i < map_catalog_count() && i < MAP_CATALOG_OFFICIAL_COUNT; i++) {
        const MapCatalogEntry *e = map_catalog_at(i);
        if (!e || !e->name) continue;
        snprintf(value, sizeof(value), "%zu", i);
        option(sb, value, e->name, (int) i == map);
    }
    sb_append(sb, "</select></div>");
#ifndef __EMSCRIPTEN__
    server_type_html(sb);
#endif
    sb_append(sb, "</div>");
}

char *seek_prefs_extend_tab(const char *tab) {
    // the General tab (it holds Localization): the section goes last, so v10's own rows keep their
    // places (a KrunkNative addition, like the account's Backup Settings row)
    if (!tab || !strstr(tab, "id=\"setBod_local\"") || strstr(tab, "id=\"setHed_quickplay\"")) return NULL;
    StrBuf sb = {0};
    sb_append(&sb, tab);
    section_html(&sb);
    return sb_finish(&sb);
}

char *seek_prefs_extend_basic_tab(const char *tab) {
#ifdef __EMSCRIPTEN__
    (void) tab;
    return NULL;
#else
#ifdef UI_SCENARIO_SUPPORTED
    if (ui_scenario_active()) return NULL; // uidiff compares the tab with v10's (v10/settings-general)
#endif
    // v10's basic General tab: Localization, then Server Browser & Matchmaking (Legacy Browser,
    // Default Region Only): the row goes last in that folder
    const char *bod = tab ? strstr(tab, "id=\"setBod_serverb\"") : NULL;
    if (!bod || strstr(tab, "id=\"knSeekServers\"")) return NULL;
    const char *p = strchr(bod, '>');
    int depth = 1; // inside the folder's <div>
    while (p && depth > 0) {
        const char *open = strstr(p + 1, "<div"), *close = strstr(p + 1, "</div>");
        if (!close) return NULL;
        if (open && open < close) {
            depth++;
            p = open;
        } else {
            depth--;
            p = close;
        }
    }
    if (!p) return NULL;
    StrBuf sb = {0};
    sb_append_n(&sb, tab, (size_t) (p - tab));
    server_type_html(&sb);
    sb_append(&sb, p); // the folder's own </div> onwards
    return sb_finish(&sb);
#endif
}
