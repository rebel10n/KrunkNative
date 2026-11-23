// Krunker v10.0.7's disconnect report (client/include/disconnect_report.h; deob-10.0.7.js:229972-229997).
//
//   window.disconnectPopup = (code, reason) => genericPopup("<div class='imageListHeader' ...>Report</div>...
//       <div class='mapFndB' onclick='window.getDCExport(this, &quot;code&quot;, &quot;reason&quot;)' ...>Copy Data</div>",
//       "disconnectPop");
//   window.getDCExport = (el, code, reason) => { copy(JSON.stringify({ua, savedRegion, connectedRegion, connectedHost,
//       ping, v, steam, client, mobile, browser, code, reason, time}, null, 2)); el.textContent = "Copied";
//       setTimeout(() => el.textContent = <the text before>, 1250) }
//
// The values, as Box Office has them:
//   ua              navigator.userAgent (startup_user_agent: natively "Mozilla/5.0 (<OS>; <arch>) KrunkNative/<commit>")
//   savedRegion     SETTINGS.list.defaultRegion.val (ui_setting_value)
//   connectedRegion the game's region (setGameInstanceId's fetchGameInfo, :226570-226580; servers_ui_seek_region, or the
//                   official game's krunker_net_region), null when none
//   connectedHost   the game server's host (:229297, the seek's host; Client.net_address), null when none
//   ping            the last PING_RESULT (:227269; Client.net_ping_ms), -1 before one
//   v               clientVersion "10.0.7r1" (:16393)
//   steam, client   the Steam build / the Electron client (:204196, :204252): false, Box Office is neither
//   mobile          window._isMobile (ui_mobile_active)
//   browser         ua.slice(ua.lastIndexOf(") ") + 2)
//   time            new Date().toString()
// The text says to send it to support; v10 names no address there, so neither does Box Office (the menu's Contact,
// windows[20] tab 1, is where its support is).
#include <disconnect_report.h>
#include <startup_banner.h>
#include <krunker_build.h>
#include <krunker_net.h>
#include <backend_ui.h>
#include <ui_logic.h>
#include <ui_mobile.h>
#include <html_dom.h>
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define DC_COPIED_MS 1250.0 // setTimeout(..., 0x4e2)

typedef struct {
    HtmlElem el;
    char *text;
} CopiedReset;

static CopiedReset g_reset;
static int g_reset_timer;

static void copied_reset(void *user) {
    (void) user;
    g_reset_timer = 0;
    if (g_reset.el && g_reset.text) html_dom_set_text(g_reset.el, g_reset.text);
    free(g_reset.text);
    g_reset.text = NULL;
    g_reset.el = 0;
}

// new Date().toString(): "Mon Nov 23 2026 00:30:00 GMT+0000 (Coordinated Universal Time)" on the web; natively the
// same fields from the C library (its zone name, e.g. "UTC")
static void date_string(char *out, const size_t n) {
#ifdef __EMSCRIPTEN__
    char *s = (char *) EM_ASM_PTR({ return stringToNewUTF8(new Date().toString()); });
    snprintf(out, n, "%s", s ? s : "");
    free(s);
#else
    const time_t now = time(NULL);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char zone[64] = "";
    strftime(zone, sizeof(zone), "%Z", &tm);
    char base[96] = "";
    strftime(base, sizeof(base), "%a %b %d %Y %H:%M:%S GMT%z", &tm);
    snprintf(out, n, "%s (%s)", base, zone);
#endif
}

static void add_str_or_null(cJSON *o, const char *key, const char *v) {
    if (v && v[0]) cJSON_AddStringToObject(o, key, v);
    else cJSON_AddNullToObject(o, key);
}

// JSON.stringify(value, null, 2) of a flat object: two spaces before each member, ": " after its key
static char *stringify2(const cJSON *o) {
    StrBuf sb = {0};
    sb_append(&sb, "{");
    const cJSON *m;
    int first = 1;
    cJSON_ArrayForEach(m, o) {
        cJSON *key = cJSON_CreateString(m->string);
        char *k = cJSON_PrintUnformatted(key), *v = cJSON_PrintUnformatted(m);
        sb_appendf(&sb, "%s\n  %s: %s", first ? "" : ",", k ? k : "\"\"", v ? v : "null");
        free(k);
        free(v);
        cJSON_Delete(key);
        first = 0;
    }
    sb_append(&sb, first ? "}" : "\n}");
    return sb_finish(&sb);
}

char *disconnect_report_json(const char *code, const char *reason) {
    char ua[256];
    startup_user_agent(ua, sizeof(ua));
    const MenuUi *m = menu_ui();
    const Client *client = m ? m->client : NULL;
    const int official = krunker_net_session_game()[0] != 0;
    const char *region = official ? krunker_net_region() : servers_ui_seek_region();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "ua", ua);
    const cJSON *saved = ui_setting_value("defaultRegion");
    cJSON_AddItemToObject(o, "savedRegion", saved ? cJSON_Duplicate(saved, 1) : cJSON_CreateNull());
    add_str_or_null(o, "connectedRegion", region);
    add_str_or_null(o, "connectedHost", client ? client->net_address : NULL);
    cJSON_AddNumberToObject(o, "ping", client ? client->net_ping_ms : -1);
    cJSON_AddStringToObject(o, "v", KRUNKER_CLIENT_VERSION);
    cJSON_AddBoolToObject(o, "steam", 0);
    cJSON_AddBoolToObject(o, "client", 0);
    cJSON_AddBoolToObject(o, "mobile", ui_mobile_active() != 0);
    const char *tail = NULL; // ua.slice(ua.lastIndexOf(") ") + 2): from index 1 when ") " is missing (-1 + 2)
    for (const char *p = strstr(ua, ") "); p; p = strstr(p + 1, ") ")) tail = p + 2;
    cJSON_AddStringToObject(o, "browser", tail ? tail : ua[0] ? ua + 1 : ua);
    cJSON_AddStringToObject(o, "code", code ? code : "undefined");
    cJSON_AddStringToObject(o, "reason", reason ? reason : "undefined");
    char when[160];
    date_string(when, sizeof(when));
    cJSON_AddStringToObject(o, "time", when);
    char *s = stringify2(o);
    cJSON_Delete(o);
    return s;
}

// a value for the onclick's &quot;...&quot; JS string: backslash and quotes escaped, then the attribute's escaping
static void append_js_attr(StrBuf *sb, const char *s) {
    StrBuf js = {0};
    for (const char *c = s ? s : ""; *c; c++) {
        if (*c == '\\' || *c == '"' || *c == '\'') sb_appendf(&js, "\\%c", *c);
        else if (*c == '\n') sb_append(&js, "\\n");
        else sb_append_n(&js, c, 1);
    }
    char *t = sb_finish(&js);
    sb_append_esc(sb, t);
    free(t);
}

static void disconnect_popup(const char *code, const char *reason) {
    StrBuf sb = {0};
    sb_append(&sb, "<div class='imageListHeader' style='font-size: 26px;'>Report</div><div style='font-size:20px;color:#fff'>"
                   "<div style='margin-top: 3px;font-size: 22px;color: rgba(255, 255, 255, 0.7);'>Copy Paste this error data and send it to support.</div>"
                   "<div class='mapFndB' onclick='window.getDCExport(this, &quot;");
    append_js_attr(&sb, code ? code : "undefined");
    sb_append(&sb, "&quot;, &quot;");
    append_js_attr(&sb, reason ? reason : "undefined");
    sb_append(&sb, "&quot;)' onmouseover='playTick(0.1)' style='background-color:#fc5a4e;width: 380px;margin-top: 10px;'>Copy Data</div></div>");
    char *html = sb_finish(&sb);
    // genericPopup(html, "disconnectPop")
    ui_loadout_clear_pops();
    const HtmlElem holder = html_dom_by_id("popupHolder");
    if (holder) html_dom_set_display(holder, "block");
    const HtmlElem pop = html_dom_by_id("genericPop");
    if (pop) {
        html_dom_set_attr(pop, "class", "disconnectPop");
        html_dom_set_attr(pop, "style", "");
        html_dom_set_html(pop, html);
        html_dom_set_display(pop, "block");
    }
    free(html);
}

static void dc_export(const HtmlElem el, const char *code, const char *reason) {
    char *json = disconnect_report_json(code, reason);
    html_clipboard_write(json);
    printf("[dc] report copied (%zu bytes)\n", strlen(json));
    free(json);
    if (!el) return;
    // el.textContent = "Copied", then the text it had (a second click inside the 1250 ms keeps the first text)
    if (!g_reset_timer || g_reset.el != el) {
        if (g_reset_timer) {
            html_timer_cancel(g_reset_timer);
            copied_reset(NULL);
        }
        char before[128] = "";
        html_dom_get_text(el, before, sizeof(before));
        g_reset.el = el;
        g_reset.text = strdup(before);
    } else {
        html_timer_cancel(g_reset_timer);
    }
    html_dom_set_text(el, "Copied");
    g_reset_timer = html_timer_add(DC_COPIED_MS, copied_reset, NULL);
}

// an argument as the page's string concatenation makes it: a string, a number's digits, "true" / "false", or NULL
// (missing: the popup's "undefined")
static const char *str_arg(const HtmlArg *args, const int argc, const int i, char *buf, const size_t n) {
    if (i >= argc) return NULL;
    switch (args[i].kind) {
        case HTML_ARG_STR: return args[i].str;
        case HTML_ARG_NUM: snprintf(buf, n, "%.15g", args[i].num); return buf;
        case HTML_ARG_BOOL: return args[i].num != 0 ? "true" : "false";
        default: return NULL;
    }
}

int disconnect_report_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    if (!name) return -1;
    char a[32], b[32];
    if (!strcmp(name, "disconnectPopup")) {
        disconnect_popup(str_arg(args, argc, 0, a, sizeof(a)), str_arg(args, argc, 1, b, sizeof(b)));
        return 1;
    }
    if (!strcmp(name, "getDCExport") || !strcmp(name, "window.getDCExport")) {
        const HtmlElem el = argc > 0 && args[0].kind == HTML_ARG_THIS && ev ? ev->target : 0;
        dc_export(el, str_arg(args, argc, 1, a, sizeof(a)), str_arg(args, argc, 2, b, sizeof(b)));
        return 1;
    }
    return -1;
}
