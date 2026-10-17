// Seek preferences: Settings > General > Quickplay (client/include/seek_prefs.h,
// doc/v10/matchmaker-token.md "Seek preferences"). v10.0.2 reads `seek_modes` / `seek_maps`
// (getSavedVal, comma lists of indices; v10.js:222011-222016) into the seek's dataQuery
// dsmo / dsma (v10.js:222051-222055) and has no page writing them. The rows here are KrunkNative's,
// in v10's settings markup (setHed / setBodH folders, settName rows with the label.switch of
// Settings > General), one switch per mode and one per map. The pure rules are in seek_filter.c.
#include <seek_prefs.h>
#include <seek_filter.h> // the pure core (unit-tested)
#include <krunker_net.h> // krunker_net_in_client (the Server Type texts)
#include <ui_logic.h>
#include <krunker_maps.h>
#include <krunker_build.h> // KRUNKER_REGIONS: the matchmaker's regions
#include <krunker_lobbies.h> // krunker_region_name
#include <krunker_map_source.h> // the official list: names (krunker_maps.h)
#include <modes.h>
#include <flavor.h> // Krunker Strike: its own servers only; flavor_get (the flavor's rotation)
#include <flavor_rules.h> // flavor_rotation_modes, flavor_mode_allowed
#include <krunker_relay.h> // the web's krunker.io state (Server Type row, first-run note)
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define SEEK_MODES_KEY "seek_modes"
#define SEEK_MAPS_KEY "seek_maps"
#define SEEK_SERVERS_KEY "knSeekServers"
#define SEEK_ON_LAUNCH_KEY "knSeekOnLaunch"
#define KRUNKER_REGION_KEY "knKrunkerRegion"

static int list_contains(const int *list, const int count, const int v) {
    for (int i = 0; i < count; i++) {
        if (list[i] == v) return 1;
    }
    return 0;
}

// a mode the switches offer: named, and played by this flavor (Krunker Strike: FRVR's modes)
static int mode_valid(const int id) {
    const GameModeConfig *m = id >= 0 ? mode_config(id) : NULL;
    return m && m->name && m->alias && flavor_mode_allowed(id);
}
static int map_valid(const int index) { return index >= 0 && index < KRUNKER_OFFICIAL_MAP_COUNT && krunker_official_map_name(index); }

// the switched-on entries of a saved list, as v10's _69484 reads it (seek_filter_parse); 0 = none
static int saved_list(const char *key, int *out, const int max, int (*valid)(int)) {
    return seek_filter_parse(ui_setting_saved(key), out, max < SEEK_FILTER_MAX ? max : SEEK_FILTER_MAX, valid);
}

// Krunker's public rotation, the default of each list: v10's rotationModes of this flavor, and v10's
// rotationMaps (seek_filter_rotation_maps)
static int rotation_modes(int *out, const int max) {
    const int *pool = NULL;
    const size_t count = flavor_rotation_modes(flavor_get(), &pool);
    int n = 0;
    for (size_t i = 0; pool && i < count && n < max; i++) out[n++] = pool[i];
    return n;
}

static int rotation_maps(int *out, const int max) {
    const int *pool = NULL;
    const size_t count = seek_filter_rotation_maps(&pool);
    int n = 0;
    for (size_t i = 0; pool && i < count && n < max; i++) out[n++] = pool[i];
    return n;
}

// The lists a seek uses: the switched-on entries, or the rotation when none is on
static void current_filter(SeekFilter *f) {
    int on_modes[SEEK_FILTER_MAX], def_modes[SEEK_FILTER_MAX], on_maps[SEEK_FILTER_MAX], def_maps[SEEK_FILTER_MAX];
    const int on_mode_count = saved_list(SEEK_MODES_KEY, on_modes, SEEK_FILTER_MAX, mode_valid);
    const int on_map_count = saved_list(SEEK_MAPS_KEY, on_maps, SEEK_FILTER_MAX, map_valid);
    const int def_mode_count = rotation_modes(def_modes, SEEK_FILTER_MAX);
    const int def_map_count = rotation_maps(def_maps, SEEK_FILTER_MAX);
    memset(f, 0, sizeof(*f));
    f->mode_count = seek_filter_effective(on_modes, on_mode_count, def_modes, def_mode_count, f->modes, SEEK_FILTER_MAX);
    f->modes_chosen = on_mode_count > 0;
    f->map_count = seek_filter_effective(on_maps, on_map_count, def_maps, def_map_count, f->maps, SEEK_FILTER_MAX);
    for (int i = 0; i < f->map_count; i++) f->map_names[i] = krunker_official_map_name(f->maps[i]);
}

int seek_prefs_filtering(void) {
    int tmp[SEEK_FILTER_MAX];
    return saved_list(SEEK_MODES_KEY, tmp, SEEK_FILTER_MAX, mode_valid) > 0 || saved_list(SEEK_MAPS_KEY, tmp, SEEK_FILTER_MAX, map_valid) > 0;
}

// The Server Type as saved (knSeekServers): what the Quickplay row shows. Krunker Strike seeks its own
// servers only (the coordinator's Strike pool): official krunker.io lobbies are Box Office's
// (krunker_lobbies_enabled), whatever a shared config says.
static SeekServers saved_servers(void) {
    if (flavor_get() != FLAVOR_BASE) return SEEK_SERVERS_KN;
    const char *s = cJSON_GetStringValue(ui_setting_saved(SEEK_SERVERS_KEY));
    if (s && !strcmp(s, "official")) return SEEK_SERVERS_OFFICIAL;
    if (s && !strcmp(s, "both")) return SEEK_SERVERS_BOTH;
    return SEEK_SERVERS_KN;
}

SeekServers seek_prefs_servers(void) {
#ifdef __EMSCRIPTEN__
    // the web's seeks stay on Box Office servers: the krunker.io join from the browser (over the
    // bridge's relay, krunker_relay.h) is not built, so the saved choice waits for it and the row says so
    return SEEK_SERVERS_KN;
#else
    return saved_servers();
#endif
}


const char *seek_prefs_krunker_region(void) {
#ifdef __EMSCRIPTEN__
    return ""; // the web client never seeks krunker.io
#else
    static const char *const regions[] = {KRUNKER_REGIONS};
    const char *s = cJSON_GetStringValue(ui_setting_saved(KRUNKER_REGION_KEY));
    for (size_t i = 0; s && i < sizeof(regions) / sizeof(regions[0]); i++) {
        if (!strcmp(s, regions[i])) return regions[i];
    }
    return "";
#endif
}

void seek_prefs_set_servers(const SeekServers servers) {
    ui_settings_set_string(SEEK_SERVERS_KEY, servers == SEEK_SERVERS_OFFICIAL ? "official" : servers == SEEK_SERVERS_BOTH ? "both" : "kn");
}

// The switch stores a boolean (checkbox onclick, setSetting); a script's 0 / "0" / "false" counts as
// off too. Unset (no default in the tables) is on.
int seek_prefs_on_launch(void) {
    const cJSON *v = ui_setting_value(SEEK_ON_LAUNCH_KEY);
    if (!v) return 1;
    if (cJSON_IsFalse(v) || (cJSON_IsNumber(v) && v->valuedouble == 0)) return 0;
    const char *s = cJSON_GetStringValue(v);
    return !(s && (!strcmp(s, "false") || !strcmp(s, "0")));
}


void seek_prefs_dq_fields(char *out, const size_t n) {
    SeekFilter f;
    current_filter(&f);
    seek_filter_dq_fields(&f, out, n);
}

void seek_prefs_data_query(char *out, const size_t n) {
    if (!out || !n) return;
    char fields[1024];
    seek_prefs_dq_fields(fields, sizeof(fields));
    if (fields[0]) snprintf(out, n, "{%s}", fields + 1);
    else out[0] = 0;
}

int seek_prefs_match(const int mode, const char *map_name) { return seek_prefs_lobby_ok(mode, map_name, 0); }

int seek_prefs_flag(const void *value) { return seek_filter_flag(value); }

int seek_prefs_lobby_ok(const int mode, const char *map_name, const unsigned flags) {
    SeekFilter f;
    current_filter(&f);
    return seek_filter_lobby_ok(&f, mode, map_name, flags);
}

// "a, b, c" (the first three, then "+N more"), or `none` for an empty list
static void names_list(char *out, const size_t n, const int *ids, const int count, const int maps) {
    int len = 0;
    for (int i = 0; i < count && i < 3 && (size_t) len < n; i++) {
        const GameModeConfig *m = maps ? NULL : mode_config(ids[i]);
        const char *name = maps ? krunker_official_map_name(ids[i]) : m ? m->name : NULL;
        len += snprintf(out + len, n - (size_t) len, "%s%s", i ? ", " : "", name ? name : "?");
    }
    if (count > 3 && (size_t) len < n) {
        char more[16], text[48];
        snprintf(more, sizeof(more), "%d", count - 3);
        i18n_trf(text, sizeof(text), "kn.quickplay.more", "+{0} more", 1, I18N_ARGV(more));
        snprintf(out + len, n - (size_t) len, " %s", text);
    }
}

void seek_prefs_describe(char *out, const size_t n) {
    SeekFilter f;
    current_filter(&f);
    int on_modes[SEEK_FILTER_MAX], on_maps[SEEK_FILTER_MAX];
    const int mode_on = saved_list(SEEK_MODES_KEY, on_modes, SEEK_FILTER_MAX, mode_valid) > 0;
    const int map_on = saved_list(SEEK_MAPS_KEY, on_maps, SEEK_FILTER_MAX, map_valid) > 0;
    char mode_text[160], map_text[160];
    if (mode_on) names_list(mode_text, sizeof(mode_text), f.modes, f.mode_count, 0);
    else snprintf(mode_text, sizeof(mode_text), "%s", I18N("kn.quickplay.publicModes", "public modes"));
    if (map_on) names_list(map_text, sizeof(map_text), f.maps, f.map_count, 1);
    else snprintf(map_text, sizeof(map_text), "%s", I18N("kn.quickplay.publicMaps", "public maps"));
    i18n_trf(out, n, "kn.quickplay.describe", "{0} on {1}", 2, I18N_ARGV(mode_text, map_text));
}

static void option(StrBuf *sb, const char *value, const char *label, const int selected) {
    sb_append(sb, "<option value=\"");
    sb_append_esc(sb, value);
    sb_append(sb, selected ? "\" selected=\"\">" : "\">");
    sb_append_esc(sb, label);
    sb_append(sb, "</option>");
}

// The web's Server Type row (seek_prefs_servers: the seeks stay on Box Office servers): the saved choice
// in the same select, and krunker.io's state said plainly under it (krunker_relay.h). Its change is
// saved like the native row's; nothing on the web seeks krunker.io until the relay join is built.
static void server_type_web_html(StrBuf *sb) {
    const SeekServers servers = saved_servers();
    sb_append(sb, "<div class=\"settName\" title=\"");
    sb_append_esc(sb, I18N("kn.quickplay.serverType.infoWeb", "What Quick Match and Find New Game (F4) look for. This browser plays on Box Office servers"));
    sb_append(sb, "\">");
    sb_append_esc(sb, I18N("kn.quickplay.serverType", "Server Type"));
    sb_append(sb, " <select id=\"knSeekServers\" onchange=\"window.setSetting(&quot;" SEEK_SERVERS_KEY "&quot;, this.value)\" class=\"inputGrey2\">");
    option(sb, "kn", I18N("kn.quickplay.serverType.boxOffice", "Box Office servers"), servers == SEEK_SERVERS_KN);
    option(sb, "official", I18N("kn.quickplay.serverType.official", "krunker.io servers"), servers == SEEK_SERVERS_OFFICIAL);
    option(sb, "both", I18N("kn.quickplay.serverType.both", "Both"), servers == SEEK_SERVERS_BOTH);
    sb_append(sb, "</select></div><div class=\"settName\" id=\"knKioRelayNote\" style=\"color:rgba(255,255,255,0.75);font-size:15px\">");
    sb_append_esc(sb, krunker_relay_on()
        ? I18N("kn.kio.relayNotBuilt", "krunker.io's relay is on here, but joining krunker.io from the browser is not in this build yet. Box Office servers still work.")
        : I18N("kn.kio.relayOff", "krunker.io is not reachable from this server right now. Box Office servers still work."));
    sb_append(sb, "</div>");
}

#ifndef __EMSCRIPTEN__
// Server Type (native only: the web has its own row, server_type_web_html): what Quick Match and
// Find New Game (F4) seek into; the server browser lists both kinds whatever it says. Official
// lobbies are joined in this client (krunker_net.h) unless the browser opt-out is set.
static void server_type_html(StrBuf *sb) {
    const SeekServers servers = saved_servers();
    const int here = krunker_net_in_client();
    sb_append(sb, "<div class=\"settName\" title=\"");
    sb_append_esc(sb, here ? I18N("kn.quickplay.serverType.infoJoin", "What Quick Match and Find New Game (F4) look for. The server browser lists both; "
                                                                       "krunker.io servers are joined in this client")
                           : I18N("kn.quickplay.serverType.infoBrowser", "What Quick Match and Find New Game (F4) look for. The server browser lists both; "
                                                                          "krunker.io servers open in your browser"));
    sb_append(sb, "\">");
    sb_append_esc(sb, I18N("kn.quickplay.serverType", "Server Type"));
    sb_append(sb, " <select id=\"knSeekServers\" onchange=\"window.setSetting(&quot;" SEEK_SERVERS_KEY "&quot;, this.value)\" class=\"inputGrey2\">");
    option(sb, "kn", I18N("kn.quickplay.serverType.boxOffice", "Box Office servers"), servers == SEEK_SERVERS_KN);
    option(sb, "official", I18N("kn.quickplay.serverType.official", "krunker.io servers"), servers == SEEK_SERVERS_OFFICIAL);
    option(sb, "both", I18N("kn.quickplay.serverType.both", "Both"), servers == SEEK_SERVERS_BOTH);
    sb_append(sb, "</select></div><div class=\"settName\" style=\"color:rgba(255,255,255,0.5);font-size:15px\">");
    sb_append_esc(sb, here ? I18N("kn.quickplay.serverType.noteJoin", "Quick Match and F4 seek into these. krunker.io servers are joined in this client with your "
                                                                       "krunker.io account (krunker.io may ask for a security check first). Both prefers a Box Office server when one matches.")
                           : I18N("kn.quickplay.serverType.noteBrowser", "Quick Match and F4 seek into these. krunker.io servers open in your browser. "
                                                                          "Both prefers a Box Office server when one matches."));
    sb_append(sb, "</div>");
}

// krunker.io Region: v10's Default Region for the official lobbies (the matchmaker's regions by
// Krunker's own place names, v10 _7517), in the same select markup as Server Type
static void krunker_region_html(StrBuf *sb) {
    static const char *const regions[] = {KRUNKER_REGIONS};
    // the player's choice, else the region the ping race picked (v10's Region row shows the ping cache)
    const char *current = seek_prefs_krunker_region()[0] ? seek_prefs_krunker_region() : krunker_seek_region_auto();
    sb_append(sb, "<div class=\"settName\" title=\"");
    sb_append_esc(sb, I18N("kn.quickplay.kioRegion.info", "The region Quick Match, Find New Game (F4) and krunker.io's matchmaker look in for krunker.io servers"));
    sb_append(sb, "\">");
    sb_append_esc(sb, I18N("kn.quickplay.kioRegion", "krunker.io Region"));
    sb_append(sb, " <select id=\"knKrunkerRegion\" onchange=\"window.setSetting(&quot;" KRUNKER_REGION_KEY "&quot;, this.value)\" class=\"inputGrey2\">");
    option(sb, "", I18N("kn.quickplay.kioRegion.default", "Default"), !current[0]);
    for (size_t i = 0; i < sizeof(regions) / sizeof(regions[0]); i++) option(sb, regions[i], krunker_region_name(regions[i]), !strcmp(current, regions[i]));
    sb_append(sb, "</select></div>");
}
#endif

// Auto Seek on Launch: a switch row like v10's settings switches (onclick setSetting, checked when on)
static void launch_seek_html(StrBuf *sb) {
    sb_append(sb, "<div class=\"settName\" title=\"");
    sb_append_esc(sb, I18N("kn.quickplay.autoSeek.info", "When the game starts, seek a game with the Server Type, Seek Modes and Seek Maps, as Find New Game (F4) does"));
    sb_append(sb, "\">");
    sb_append_esc(sb, I18N("kn.quickplay.autoSeek", "Auto Seek on Launch"));
    sb_append(sb, " <label class=\"switch\" style=\"margin-left:10px\"><input type=\"checkbox\" onclick=\"window.setSetting(&quot;" SEEK_ON_LAUNCH_KEY
                  "&quot;, this.checked)\"");
    if (seek_prefs_on_launch()) sb_append(sb, " checked=\"\"");
    sb_append(sb, "><span class=\"slider\"><span class=\"grooves\"></span></span></label></div>");
}

// One switch row (v10's settName with the label.switch of Settings > General): on when the index is
// in the effective list. maps: the map list's rows, else the mode list's.
static void switch_row(StrBuf *sb, const int maps, const int index, const char *label, const int on) {
    char buf[256];
    const char *key = maps ? SEEK_MAPS_KEY : SEEK_MODES_KEY;
    const char *prefix = maps ? "knSeekMap" : "knSeekMode";
    sb_append(sb, "<div class=\"settName\">");
    sb_append_esc(sb, label);
    snprintf(buf, sizeof(buf),
             " <label class=\"switch\" style=\"margin-left:10px\"><input id=\"%s%d\" type=\"checkbox\" "
             "onclick=\"knSeekSwitch(&quot;%s&quot;, %d, this.checked)\"%s><span class=\"slider\"><span class=\"grooves\"></span></span></label></div>",
             prefix, index, key, index, on ? " checked=\"\"" : "");
    sb_append(sb, buf);
}

// One folder of switches: a collapsible setHed like v10's settings folders (collapseFolder needs the
// setHed_ / setBod_ id pair). The modes are open at first (the short list the player picks from), the
// maps closed (46 rows).
static void folder_html(StrBuf *sb, const int maps, const char *title, const char *note) {
    const char *id = maps ? "quickplay_maps" : "quickplay_modes";
    const int open = !maps;
    char buf[160];
    snprintf(buf, sizeof(buf), "<div class=\"setHed\" id=\"setHed_%s\" onclick=\"window.windows[0].collapseFolder(this)\">", id);
    sb_append(sb, buf);
    sb_append(sb, open ? "<span class=\"material-icons plusOrMinus\">keyboard_arrow_down</span> " : "<span class=\"material-icons plusOrMinus\">keyboard_arrow_right</span> ");
    sb_append_esc(sb, title);
    sb_append(sb, "</div>");
    snprintf(buf, sizeof(buf), "<div class=\"setBodH\" id=\"setBod_%s\"%s>", id, open ? "" : " style=\"display:none\"");
    sb_append(sb, buf);
    snprintf(buf, sizeof(buf), "<div class=\"settName\" id=\"knSeek%sNote\" style=\"color:rgba(255,255,255,0.5);font-size:15px\">", maps ? "Map" : "Mode");
    sb_append(sb, buf);
    sb_append_esc(sb, note);
    sb_append(sb, "</div>");

    SeekFilter f;
    current_filter(&f);
    const int n = maps ? KRUNKER_OFFICIAL_MAP_COUNT : MODE_COUNT;
    for (int i = 0; i < n; i++) {
        const char *name = maps ? (map_valid(i) ? krunker_official_map_name(i) : NULL) : (mode_valid(i) ? mode_config(i)->name : NULL);
        if (!name) continue;
        char shown[96]; // v10's _8933: the first letter upper-cased (v10.js:23847)
        snprintf(shown, sizeof(shown), "%s", name);
        if (!maps && shown[0] >= 'a' && shown[0] <= 'z') shown[0] = (char) (shown[0] - 'a' + 'A');
        const int on = maps ? list_contains(f.maps, f.map_count, i) : list_contains(f.modes, f.mode_count, i);
        switch_row(sb, maps, i, shown, on);
    }
    sb_append(sb, "</div>");
}

// "Nothing switched on: the public rotation" notes name what a seek then looks for
static void pool_note(char *out, const size_t n) {
    int pool[SEEK_FILTER_MAX];
    const int count = rotation_modes(pool, SEEK_FILTER_MAX);
    char names[400] = "";
    names_list(names, sizeof(names), pool, count, 0);
    i18n_trf(out, n, "kn.quickplay.seekModes.note",
             "Krunker's public rotation is on by default: {0}. Switch on Trade, Parkour or any other mode to seek it. With every switch off, the rotation is sought.",
             1, I18N_ARGV(names));
}

// The presets (seek_filter.h SeekPreset): one button each, above the folded section (always shown). A
// button writes its lists (knSeekPreset, below); its title names what it switches on.
static const char *preset_name(const int preset) {
    switch (preset) {
    case SEEK_PRESET_ROTATION: return I18N("kn.quickplay.preset.rotation", "Krunker rotation");
    case SEEK_PRESET_TEAM: return I18N("kn.quickplay.preset.team", "Team modes");
    case SEEK_PRESET_FFA: return I18N("kn.quickplay.preset.ffa", "Free for All");
    case SEEK_PRESET_RANKED: return I18N("kn.quickplay.preset.ranked", "Competitive");
    case SEEK_PRESET_ANY: return I18N("kn.quickplay.preset.any", "Every mode and map");
    default: return "";
    }
}

static const char *preset_info(const int preset) {
    switch (preset) {
    case SEEK_PRESET_ROTATION:
        return I18N("kn.quickplay.preset.rotation.info", "Krunker's public rotation of modes and maps: what Quick Play seeks with every switch off");
    case SEEK_PRESET_TEAM:
        return I18N("kn.quickplay.preset.team.info", "Team Deathmatch, Hardpoint, Capture the Flag, Kill Confirmed, Domination and Squad Deathmatch, on the rotation maps");
    case SEEK_PRESET_FFA:
        return I18N("kn.quickplay.preset.ffa.info", "Free for All, Classic FFA, Gun Game, Last Man Standing, Kranked FFA and Bighead FFA, on the rotation maps");
    case SEEK_PRESET_RANKED:
        return I18N("kn.quickplay.preset.ranked.info", "Hardpoint, the mode of Ranked, on the Ranked map pool: Sandstorm, Undergrowth, Industry, Site, Bureau, Eterno Simulator and Frontier");
    case SEEK_PRESET_ANY:
        return I18N("kn.quickplay.preset.any.info", "Every mode and every official map: any public lobby");
    default:
        return "";
    }
}

static void presets_html(StrBuf *sb) {
    sb_append(sb, "<div class=\"knSeekPresets\"><span class=\"knSeekPresetsLbl\">");
    sb_append_esc(sb, I18N("kn.quickplay.presets", "Presets"));
    sb_append(sb, "</span>");
    for (int i = 0; i < SEEK_PRESET_COUNT; i++) {
        char buf[96];
        snprintf(buf, sizeof(buf), "<div class=\"knSeekPreset\" id=\"knSeekPreset%d\" onclick=\"knSeekPreset(%d)\" title=\"", i, i);
        sb_append(sb, buf);
        sb_append_esc(sb, preset_info(i));
        sb_append(sb, "\">");
        sb_append_esc(sb, preset_name(i));
        sb_append(sb, "</div>");
    }
    sb_append(sb, "</div>");
}

// The Server Type rows (the web's own, or the native Server Type and krunker.io Region)
static void server_rows_html(StrBuf *sb) {
#ifdef __EMSCRIPTEN__
    server_type_web_html(sb);
#else
    server_type_html(sb);
    krunker_region_html(sb);
#endif
}

// The section: its header (a chevron, collapsed by default; collapseFolder toggles the body; the state is
// the page's, never saved), the presets row, then the body with the switches
static void section_html(StrBuf *sb, const int server_rows) {
    sb_append(sb, "<div class=\"setHed\" id=\"setHed_quickplay\" onclick=\"window.windows[0].collapseFolder(this)\">"
                  "<span class=\"material-icons plusOrMinus\">keyboard_arrow_right</span> ");
    sb_append_esc(sb, I18N("kn.quickplay.header", "Quickplay"));
    sb_append(sb, "</div>");
    presets_html(sb);
    sb_append(sb, "<div class=\"setBodH\" id=\"setBod_quickplay\" style=\"display:none\">");
    if (server_rows) server_rows_html(sb);
    launch_seek_html(sb);
    char note[600];
    pool_note(note, sizeof(note));
    folder_html(sb, 0, I18N("kn.quickplay.seekModes", "Seek Modes"), note);
    folder_html(sb, 1, I18N("kn.quickplay.seekMaps", "Seek Maps"),
                I18N("kn.quickplay.seekMaps.note", "Krunker's public rotation of maps is on by default. With every switch off, the rotation is sought."));
    sb_append(sb, "</div>");
}

// Saves one of the lists as the switches write it ("" = none: the rotation)
static void save_list(const char *key, const int *list, const int count) {
    char text[512] = "";
    seek_filter_list_text(list, count, text, sizeof(text));
    printf("[ui] Quickplay %s=%s\n", key, text[0] ? text : "(none: the public rotation)");
    ui_settings_set_string(key, text);
}

// knSeekSwitch(key, index, checked): the saved list with that switch flipped (the rotation when none
// was switched on), ascending, saved as a comma list ("" when every switch is off: the rotation is sought)
static void save_switch(const char *key, const int maps, const int index, const int checked) {
    int on[SEEK_FILTER_MAX], def[SEEK_FILTER_MAX], out[SEEK_FILTER_MAX];
    const int on_count = saved_list(key, on, SEEK_FILTER_MAX, maps ? map_valid : mode_valid);
    const int def_count = maps ? rotation_maps(def, SEEK_FILTER_MAX) : rotation_modes(def, SEEK_FILTER_MAX);
    const int n = seek_filter_toggle(on, on_count, def, def_count, index, checked, out, SEEK_FILTER_MAX);
    save_list(key, out, n);
}

// One switch's checkbox shows `on` (the page's element; none when the page has no such row)
static void set_switch(const int maps, const int index, const int on) {
    char id[32];
    snprintf(id, sizeof(id), "%s%d", maps ? "knSeekMap" : "knSeekMode", index);
    const HtmlElem el = html_dom_by_id(id);
    if (el) html_dom_set_checked(el, on);
}

// knSeekPreset(n): the preset's lists saved as the switches save them (its entries this flavor plays only),
// then every switch shown to match the effective lists (the rotation when none is on)
static void apply_preset(const int preset) {
    SeekPresetLists p;
    if (!seek_preset_lists(preset, &p)) return;
    int modes[SEEK_FILTER_MAX], maps[SEEK_FILTER_MAX], mode_n = 0, map_n = 0;
    for (int i = 0; i < p.mode_count; i++) {
        if (mode_valid(p.modes[i])) modes[mode_n++] = p.modes[i];
    }
    for (int i = 0; i < p.map_count; i++) {
        if (map_valid(p.maps[i])) maps[map_n++] = p.maps[i];
    }
    save_list(SEEK_MODES_KEY, modes, mode_n);
    save_list(SEEK_MAPS_KEY, maps, map_n);
    SeekFilter f;
    current_filter(&f);
    for (int i = 0; i < MODE_COUNT; i++) {
        if (mode_valid(i)) set_switch(0, i, list_contains(f.modes, f.mode_count, i));
    }
    for (int i = 0; i < KRUNKER_OFFICIAL_MAP_COUNT; i++) {
        if (map_valid(i)) set_switch(1, i, list_contains(f.maps, f.map_count, i));
    }
}

int seek_prefs_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    if (name && !strcmp(name, "knSeekPreset")) { // knSeekPreset(n): n = the preset's index (seek_filter.h)
        const double n = argc >= 1 ? args[0].num : -1.0;
        if (n >= 0 && n < SEEK_PRESET_COUNT && n == (double) (int) n) apply_preset((int) n);
        return 1;
    }
    if (!name || strcmp(name, "knSeekSwitch")) return -1;
    if (argc < 3 || args[0].kind != HTML_ARG_STR || !args[0].str) return 1;
    const char *key = args[0].str;
    const int maps = !strcmp(key, SEEK_MAPS_KEY);
    if (!maps && strcmp(key, SEEK_MODES_KEY)) return 1;
    const int index = (int) args[1].num;
    if ((double) index != args[1].num || !(maps ? map_valid(index) : mode_valid(index))) return 1;
    const int checked = args[2].kind == HTML_ARG_THIS_CHECKED ? (ev && ev->checked) : args[2].num != 0.0;
    save_switch(key, maps, index, checked);
    return 1;
}

char *seek_prefs_extend_tab(const char *tab) {
    // the General tab (it holds Localization): the section goes before the Experimental folder, so the
    // tab still ends with v10's own rows (the developer console switch is the last one: its test scrolls to
    // the bottom); a KrunkNative addition, like the account's Backup Settings row
    if (!tab || !strstr(tab, "id=\"setBod_local\"") || strstr(tab, "id=\"setHed_quickplay\"")) return NULL;
    StrBuf sb = {0};
    const char *exp = strstr(tab, "<div class=\"setHed\" id=\"setHed_experimental\"");
    if (!exp) exp = strstr(tab, "id=\"setHed_experimental\"");
    if (exp) {
        const char *open = exp;
        while (open > tab && *open != '<') open--; // the folder's own <div
        sb_append_n(&sb, tab, (size_t) (open - tab));
        section_html(&sb, 1);
        sb_append(&sb, open);
    } else {
        sb_append(&sb, tab);
        section_html(&sb, 1);
    }
    return sb_finish(&sb);
}

char *seek_prefs_extend_basic_tab(const char *tab) {
#ifdef UI_SCENARIO_SUPPORTED
    if (ui_scenario_active()) return NULL; // uidiff compares the tab with v10's (v10/settings-general)
#endif
    if (!tab || !strstr(tab, "id=\"setBod_local\"") || strstr(tab, "id=\"setHed_quickplay\"")) return NULL;
    StrBuf sb = {0};
    // v10's basic General tab: Localization, then Server Browser & Matchmaking (Legacy Browser,
    // Default Region Only): the Server Type / krunker.io Region rows go last in that folder
    const char *bod = strstr(tab, "id=\"setBod_serverb\"");
    const char *p = bod ? strchr(bod, '>') : NULL;
    int depth = 1; // inside the folder's <div>
    while (p && depth > 0) {
        const char *open = strstr(p + 1, "<div"), *close = strstr(p + 1, "</div>");
        if (!close) {
            p = NULL;
            break;
        }
        if (open && open < close) {
            depth++;
            p = open;
        } else {
            depth--;
            p = close;
        }
    }
    if (p && !strstr(tab, "id=\"knSeekServers\"")) {
        sb_append_n(&sb, tab, (size_t) (p - tab));
        server_rows_html(&sb);
        sb_append(&sb, p); // the folder's own </div> onwards
    } else {
        sb_append(&sb, tab);
    }
    section_html(&sb, 0); // the switches: the basic view has them too (they are what Quick Play seeks)
    return sb_finish(&sb);
}
