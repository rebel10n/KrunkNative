// Settings > Connections: the Discord row and Connected apps (Krunker v10.0.7), driven from C (client/include/account_links.h).
//
// Discord (svelte-17wd35, deob-10.0.7.js:97250-97490): getStatus on mount; Link asks /discord/link for Discord's authorize
// page, which opens in the system browser (v10 opens a popup and polls it closed, :97355-97357), while the row says
// "Waiting for Discord..." with Cancel and the status is re-read every few seconds (and at once on kn_brain's
// "discordStatus" frame) until the link shows up; Unlink asks once more ("Confirm", 3 s, :97375-97377); Join Server
// when the brain can add members (canJoin), Open Server once the account is in the guild. The results are
// discord.result.<key>: linked / unlinked / joined / already_member in green, the rest (unavailable, failed, relink,
// join_failed, and 10.0.7's deletion_pending for an account being deleted, :97384) in yellow.
//
// Connected apps (svelte-10ho5mo, :97493-97673): connectedApps on mount; each app's icon (or its first letter), its
// name (a link to its homepage), "by <developer>", "Connected <date> · Last used <date>", its scopes as oauth.scope.*,
// View activity (appActivity, toggled), and Disconnect (asked again for 4 s, then revokeApp and the list again).
#include <account_links.h>

#include <backend.h>
#include <backend_ui.h>
#include <i18n.h>
#include <krunker_account.h>

#include <cJSON.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STATUS_TTL_S 30.0     // a drawn Connections tab older than this loads again
#define WAIT_POLL_MS 3000.0   // the status while the browser link runs
#define WAIT_MAX_POLLS 200    // 10 minutes: kn_brain's link state lives 600 s (discord.c STATE_S)
#define UNLINK_CONFIRM_MS 3000.0
#define REVOKE_CONFIRM_MS 4000.0
#define RESULT_KEEP_S 10.0    // a result still shown when the tab draws again (v10 keeps 10 s across mounts, :97389)

double glfwGetTime(void);

static struct {
    cJSON *status;        // GET /discord/status's data (NULL = not loaded)
    double status_at;
    int loading, busy, waiting, polls, poll_timer;
    int confirm, confirm_timer;
    char result[32];      // discord.result.<key> on show, "" = none
    double result_at;
    unsigned gen;         // bumped by account_links_reset: answers of an older account are dropped
} g_dc;

static struct {
    cJSON *list;          // the apps (NULL = not loaded)
    double at;
    int loading, busy, kio;
    char confirm[96];     // the app whose Disconnect asks again
    int confirm_timer;
    cJSON *activity;      // {client_id: [rows] | null (loading)}
} g_apps;

static void copy_str(char *dst, const size_t n, const char *src) { snprintf(dst, n, "%s", src ? src : ""); }

static const char *jstr(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static int jtrue(const cJSON *o, const char *k) { return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, k)); }

// new Date(1000 * s).toLocaleDateString() / toLocaleString() as en-US: M/D/YYYY[, h:mm:ss AM]
static void locale_time(const double s, const int with_time, char *out, const size_t n) {
    const long long secs = (long long) floor(s);
    long long z = (secs >= 0 ? secs / 86400 : (secs - 86399) / 86400) + 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const long long doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    const int day = (int) (doy - (153 * mp + 2) / 5 + 1), mon = (int) (mp < 10 ? mp + 3 : mp - 9);
    const int year = (int) (yoe + era * 400 + (mon <= 2));
    if (!with_time) {
        snprintf(out, n, "%d/%d/%d", mon, day, year);
        return;
    }
    const long long tod = secs - (secs >= 0 ? secs / 86400 : (secs - 86399) / 86400) * 86400;
    const int h = (int) (tod / 3600), m = (int) (tod / 60 % 60), sec = (int) (tod % 60);
    snprintf(out, n, "%d/%d/%d, %d:%02d:%02d %s", mon, day, year, h % 12 ? h % 12 : 12, m, sec, h < 12 ? "AM" : "PM");
}

static int session_path(char *out, const size_t n, const char *path) {
    const char *t = account_session_token();
    if (!backend_enabled() || !t || !t[0]) return 0;
    char enc[512];
    backend_url_encode(enc, sizeof enc, t);
    snprintf(out, n, "%s%s%stoken=%s", backend_url(), path, strchr(path, '?') ? "&" : "?", enc);
    return 1;
}

// ================================================================== Discord

static void dc_redraw(void);

static void dc_result(const char *key) {
    copy_str(g_dc.result, sizeof g_dc.result, key);
    g_dc.result_at = glfwGetTime();
}

static int dc_result_ok(const char *key) {
    return !strcmp(key, "linked") || !strcmp(key, "unlinked") || !strcmp(key, "joined") || !strcmp(key, "already_member");
}

static const char *dc_result_text(const char *key) {
    // discord.result.<key> (deob-10.0.7.js:204284); in_use names a Box Office account here
    if (!strcmp(key, "linked")) return I18N("discord.result.linked", "Discord account linked!");
    if (!strcmp(key, "unlinked")) return I18N("discord.result.unlinked", "Discord account unlinked.");
    if (!strcmp(key, "joined")) return I18N("discord.result.joined", "You joined the Krunker Discord server!");
    if (!strcmp(key, "already_member")) return I18N("discord.result.already_member", "You're already in the Krunker Discord server.");
    if (!strcmp(key, "unavailable")) return I18N("discord.result.unavailable", "Discord linking is currently unavailable.");
    if (!strcmp(key, "relink")) return I18N("discord.result.relink", "Unlink and link Discord again to join the server.");
    if (!strcmp(key, "join_failed")) return I18N("discord.result.join_failed", "Couldn't join the Discord server. Please try again.");
    if (!strcmp(key, "cancelled")) return I18N("discord.result.cancelled", "Discord linking was cancelled.");
    if (!strcmp(key, "expired")) return I18N("discord.result.expired", "The link request expired, please try again.");
    if (!strcmp(key, "in_use")) return I18N("kn.discord.result.in_use", "That Discord account is already linked to another Box Office account.");
    if (!strcmp(key, "deletion_pending"))
        return I18N("discord.result.deletion_pending",
                    "Your account is being deleted, so its Discord account can't be unlinked or changed. Cancel the deletion first.");
    return I18N("discord.result.failed", "Failed to link Discord, please try again.");
}

static void dc_stop_wait(void) {
    if (g_dc.poll_timer) html_timer_cancel(g_dc.poll_timer);
    g_dc.poll_timer = 0;
    g_dc.waiting = 0;
    g_dc.busy = 0;
    g_dc.polls = 0;
}

static void dc_load(void);

static void dc_status_done(void *user, const int status, const char *body, const size_t len) {
    if ((unsigned) (uintptr_t) user != g_dc.gen) return;
    g_dc.loading = 0;
    cJSON *j = status == 200 && body ? cJSON_ParseWithLength(body, len) : NULL;
    cJSON *d = cJSON_DetachItemFromObjectCaseSensitive(j, "data");
    cJSON_Delete(j);
    if (cJSON_IsObject(d)) {
        const int was = g_dc.status && jtrue(g_dc.status, "linked");
        cJSON_Delete(g_dc.status);
        g_dc.status = d;
        g_dc.status_at = glfwGetTime();
        if (g_dc.waiting && jtrue(d, "linked") && !was) { // the browser link finished
            dc_stop_wait();
            dc_result("linked");
        }
    } else {
        cJSON_Delete(d);
    }
    dc_redraw();
}

static void dc_load(void) {
    char url[1024];
    if (g_dc.loading || !session_path(url, sizeof url, "/discord/status")) return;
    g_dc.loading = 1;
    if (!backend_http_request("GET", url, NULL, dc_status_done, (void *) (uintptr_t) g_dc.gen)) g_dc.loading = 0;
}

static void dc_poll(void *user) {
    (void) user;
    g_dc.poll_timer = 0;
    if (!g_dc.waiting) return;
    if (++g_dc.polls > WAIT_MAX_POLLS) { // kn_brain's state is gone by now
        dc_stop_wait();
        dc_result("expired");
        dc_redraw();
        return;
    }
    dc_load();
    g_dc.poll_timer = html_timer_add(WAIT_POLL_MS, dc_poll, NULL);
}

static const char *error_code(const cJSON *j) {
    return jstr(cJSON_GetObjectItemCaseSensitive(j, "error"), "code");
}

static void dc_link_done(void *user, const int status, const char *body, const size_t len) {
    if ((unsigned) (uintptr_t) user != g_dc.gen) return;
    g_dc.busy = 0; // the waiting line takes over while the browser runs
    cJSON *j = body ? cJSON_ParseWithLength(body, len) : NULL;
    const char *url = jstr(cJSON_GetObjectItemCaseSensitive(j, "data"), "url");
    if (status == 200 && url && !strncmp(url, "http", 4)) {
        ui_open_browser(url);
        g_dc.waiting = 1;
        g_dc.polls = 0;
        if (!g_dc.poll_timer) g_dc.poll_timer = html_timer_add(WAIT_POLL_MS, dc_poll, NULL);
    } else {
        const char *code = error_code(j);
        dc_stop_wait();
        // :97352-97354, and an account being deleted can't change its link
        dc_result(code && !strcmp(code, "DISCORD_NOT_CONFIGURED") ? "unavailable"
                  : code && !strcmp(code, "DISCORD_DELETION_PENDING") ? "deletion_pending" : "failed");
    }
    cJSON_Delete(j);
    dc_redraw();
}

static void dc_join_done(void *user, const int status, const char *body, const size_t len) {
    if ((unsigned) (uintptr_t) user != g_dc.gen) return;
    g_dc.busy = 0;
    cJSON *j = body ? cJSON_ParseWithLength(body, len) : NULL;
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(j, "data");
    const char *code = error_code(j);
    // :97366-97368
    if (status == 200 && cJSON_IsObject(d)) dc_result(jtrue(d, "joined") ? "joined" : "already_member");
    else dc_result(code && !strcmp(code, "DISCORD_NEEDS_RELINK") ? "relink" : "join_failed");
    cJSON_Delete(j);
    dc_load();
    dc_redraw();
}

static void dc_unlink_done(void *user, const int status, const char *body, const size_t len) {
    if ((unsigned) (uintptr_t) user != g_dc.gen) return;
    g_dc.busy = 0;
    cJSON *j = body ? cJSON_ParseWithLength(body, len) : NULL;
    const char *code = error_code(j);
    // :97384: v10.0.7's deletion_pending
    dc_result(status == 200 && !code ? "unlinked" : code && !strcmp(code, "DISCORD_DELETION_PENDING") ? "deletion_pending" : "failed");
    cJSON_Delete(j);
    dc_load();
    dc_redraw();
}

static void dc_confirm_off(void *user) {
    (void) user;
    g_dc.confirm_timer = 0;
    g_dc.confirm = 0;
    dc_redraw();
}

static void dc_post(const char *path, const char *json, BackendHttpFn fn) {
    char url[1024];
    if (!session_path(url, sizeof url, path) || !backend_http_request("POST", url, json, fn, (void *) (uintptr_t) g_dc.gen)) {
        g_dc.busy = 0;
        g_dc.waiting = 0;
        dc_result("failed");
    }
}

// the component's markup (templates :97286-97298), into the mount
void account_links_discord_html(StrBuf *sb) {
    if (!g_dc.status || glfwGetTime() - g_dc.status_at > STATUS_TTL_S) dc_load();
    if (g_dc.result[0] && glfwGetTime() - g_dc.result_at > RESULT_KEEP_S && !g_dc.busy) g_dc.result[0] = 0;
    const cJSON *st = g_dc.status;
    const int linked = jtrue(st, "linked");
    sb_append(sb, "<div class=\"discordInfo svelte-17wd35\">");
    sb_append_esc(sb, I18N("discord.info", "Link Discord to get your Krunker roles in the Krunker Discord server and join Discord events."));
    sb_append(sb, "</div> <div class=\"settName discordRow\">");
    const char *id = jstr(st, "id"), *avatar = jstr(st, "avatar");
    if (linked && id && avatar) { // cdn.discordapp.com/avatars/<id>/<avatar>.png?size=64 (:97315)
        sb_append(sb, "<img class=\"discordAvatar svelte-17wd35\" alt=\"\" src=\"https://cdn.discordapp.com/avatars/");
        sb_append_esc(sb, id);
        sb_append(sb, "/");
        sb_append_esc(sb, avatar);
        sb_append(sb, ".png?size=64\">");
    }
    const char *shown = linked ? (jstr(st, "globalName") && jstr(st, "globalName")[0] ? jstr(st, "globalName") : jstr(st, "username")) : NULL;
    sb_append(sb, " ");
    sb_append_esc(sb, I18N("discord.account", "Discord Account"));
    sb_append(sb, " (");
    sb_append_esc(sb, shown ? shown : I18N("discord.none", "none"));
    sb_append(sb, ") ");
    if (st && linked) {
        sb_appendf(sb, "<div class=\"lnkTw discordBtn svelte-17wd35%s\" role=\"button\" tabindex=\"0\" onclick=\"knDiscordUnlink()\">",
                   g_dc.confirm ? " discordConfirm" : "");
        sb_append_esc(sb, g_dc.confirm ? I18N("discord.unlink.confirm", "Confirm") : I18N("discord.unlink", "Unlink"));
        sb_append(sb, "</div> ");
        const char *guild = jstr(st, "guildUrl");
        if (jtrue(st, "inGuild") && guild && !strncmp(guild, "https://", 8)) {
            sb_append(sb, "<a class=\"lnkTw discordBtn discordGuildBtn svelte-17wd35\" target=\"_blank\" rel=\"noopener noreferrer\" href=\"");
            sb_append_esc(sb, guild);
            sb_append(sb, "\">");
            sb_append_esc(sb, I18N("discord.guild.open", "Open Server"));
            sb_append(sb, "</a>");
        } else if (jtrue(st, "canJoin")) {
            sb_appendf(sb, "<div class=\"lnkTw discordBtn discordGuildBtn svelte-17wd35%s\" role=\"button\" tabindex=\"0\" onclick=\"knDiscordJoin()\">",
                       g_dc.busy ? " discordBusy" : "");
            sb_append_esc(sb, I18N("discord.guild.join", "Join Server"));
            sb_append(sb, "</div>");
        }
    } else if (st && jtrue(st, "configured")) {
        sb_appendf(sb, "<div class=\"lnkTw discordBtn svelte-17wd35%s\" role=\"button\" tabindex=\"0\" onclick=\"knDiscordLink()\">",
                   g_dc.busy || g_dc.waiting ? " discordBusy" : "");
        sb_append_esc(sb, I18N("discord.link", "Link"));
        sb_append(sb, "</div>");
    }
    sb_append(sb, "</div> ");
    if (g_dc.waiting) {
        sb_append(sb, "<div class=\"discordMsg svelte-17wd35\">");
        sb_append_esc(sb, I18N("discord.linking", "Waiting for Discord..."));
        sb_append(sb, " <span class=\"discordCancel svelte-17wd35\" role=\"button\" tabindex=\"0\" onclick=\"knDiscordCancel()\">");
        sb_append_esc(sb, I18N("discord.link.cancel", "Cancel"));
        sb_append(sb, "</span></div>");
    } else if (g_dc.result[0]) {
        sb_appendf(sb, "<div class=\"discordMsg svelte-17wd35%s\">", dc_result_ok(g_dc.result) ? " discordOk" : "");
        sb_append_esc(sb, dc_result_text(g_dc.result));
        sb_append(sb, "</div>");
    } else if (!st || (!jtrue(st, "configured") && !linked)) { // not loaded (no kn_brain) or no Discord app: :97480-97483
        sb_append(sb, "<div class=\"discordMsg svelte-17wd35\">");
        sb_append_esc(sb, I18N("discord.result.unavailable", "Discord linking is currently unavailable."));
        sb_append(sb, "</div>");
    }
}

static void dc_redraw(void) {
    const HtmlElem m = html_dom_by_id("discordLinkMount");
    if (!m) return;
    StrBuf sb = {0};
    const int was_loading = g_dc.loading;
    g_dc.loading = 1; // (drawing must not start another load)
    account_links_discord_html(&sb);
    g_dc.loading = was_loading;
    char *html = sb_finish(&sb);
    html_dom_set_html(m, html);
    free(html);
}

// ================================================================== Connected apps

static void apps_redraw(void);

static void apps_done_list(cJSON *data) {
    g_apps.loading = 0;
    if (cJSON_IsArray(data)) {
        cJSON_Delete(g_apps.list);
        g_apps.list = cJSON_Duplicate(data, 1);
        g_apps.at = glfwGetTime();
    } else if (!g_apps.list) {
        g_apps.list = cJSON_CreateArray(); // v10: connectedApps().data ?? []
        g_apps.at = glfwGetTime();
    }
    apps_redraw();
}

static void apps_brain_done(void *user, const int status, const char *body, const size_t len) {
    if ((unsigned) (uintptr_t) user != g_dc.gen) return;
    cJSON *j = status == 200 && body ? cJSON_ParseWithLength(body, len) : NULL;
    apps_done_list(cJSON_GetObjectItemCaseSensitive(j, "data"));
    cJSON_Delete(j);
}

static void apps_kio_done(void *user, const int status, const cJSON *res) {
    if ((unsigned) (uintptr_t) user != g_dc.gen) return;
    apps_done_list(status == 200 ? cJSON_GetObjectItemCaseSensitive(res, "data") : NULL);
}

static void apps_load(void) {
    if (g_apps.loading) return;
    g_apps.loading = 1;
    if (g_apps.kio) { // krunker.io's own list (oauth.connectedApps, :27667): a read of the account
        if (!krunker_account_api("GET", KRUNKER_SVC_API, "/account/apps", NULL, apps_kio_done, (void *) (uintptr_t) g_dc.gen)) g_apps.loading = 0;
        return;
    }
    char url[1024];
    if (!session_path(url, sizeof url, "/account/apps") || !backend_http_request("GET", url, NULL, apps_brain_done, (void *) (uintptr_t) g_dc.gen)) {
        g_apps.loading = 0;
        if (!g_apps.list) { // no account service to ask (offline, a scenario): v10's connectedApps().data ?? []
            g_apps.list = cJSON_CreateArray();
            g_apps.at = glfwGetTime();
        }
    }
}

static const cJSON *app_by_id(const char *id) {
    const cJSON *a;
    cJSON_ArrayForEach(a, g_apps.list) {
        const char *c = jstr(a, "client_id");
        if (c && id && !strcmp(c, id)) return a;
    }
    return NULL;
}

static double jnum(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

static void app_html(StrBuf *sb, const cJSON *a) {
    const char *id = jstr(a, "client_id") ? jstr(a, "client_id") : "";
    const char *name = jstr(a, "name") ? jstr(a, "name") : "";
    char idq[200];
    { // the id as a JS string argument (the page's handlers take it back)
        size_t k = 0;
        for (const char *p = id; *p && k + 2 < sizeof idq; p++)
            if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.') idq[k++] = *p;
        idq[k] = 0;
    }
    sb_append(sb, "<div class=\"appRow svelte-10ho5mo\">");
    const char *icon = jstr(a, "icon_url");
    if (icon && !strncmp(icon, "https://", 8)) {
        sb_append(sb, "<img class=\"appIcon svelte-10ho5mo\" alt=\"\" referrerpolicy=\"no-referrer\" src=\"");
        sb_append_esc(sb, icon);
        sb_append(sb, "\">");
    } else {
        char first[8] = "?";
        if (name[0]) {
            size_t n = 1; // the first character (a UTF-8 sequence whole), upper-cased when ASCII
            while (name[n] && ((unsigned char) name[n] & 0xC0) == 0x80 && n < 4) n++;
            memcpy(first, name, n);
            first[n] = 0;
            if (first[0] >= 'a' && first[0] <= 'z') first[0] = (char) (first[0] - 32);
        }
        sb_append(sb, "<div class=\"appIcon appIconText svelte-10ho5mo\">");
        sb_append_esc(sb, first);
        sb_append(sb, "</div>");
    }
    sb_append(sb, " <div class=\"appMain svelte-10ho5mo\"><div class=\"appName svelte-10ho5mo\">");
    const char *home = jstr(a, "homepage_url");
    if (home && !strncmp(home, "https://", 8)) {
        sb_append(sb, "<a target=\"_blank\" rel=\"noopener noreferrer\" class=\"svelte-10ho5mo\" href=\"");
        sb_append_esc(sb, home);
        sb_append(sb, "\">");
        sb_append_esc(sb, name);
        sb_append(sb, "</a>");
    } else {
        sb_append_esc(sb, name);
    }
    const char *dev = jstr(a, "developer");
    if (dev && dev[0]) {
        char by[160];
        I18N_F(by, "oauth.by", "by {0}", dev);
        sb_append(sb, " <span class=\"appDev svelte-10ho5mo\">");
        sb_append_esc(sb, by);
        sb_append(sb, "</span>");
    }
    char d1[40], d2[40], authorized[120], used[120];
    locale_time(jnum(a, "created_at"), 0, d1, sizeof d1);
    I18N_F(authorized, "settings.apps.authorized", "Connected {0}", d1);
    if (jnum(a, "last_used_at") > 0) {
        locale_time(jnum(a, "last_used_at"), 0, d2, sizeof d2);
        I18N_F(used, "settings.apps.lastUsed", "Last used {0}", d2);
    } else {
        copy_str(used, sizeof used, I18N("settings.apps.never", "Never used"));
    }
    sb_append(sb, "</div> <div class=\"appMeta svelte-10ho5mo\">");
    sb_append_esc(sb, authorized);
    sb_append(sb, " · ");
    sb_append_esc(sb, used);
    sb_append(sb, "</div> <ul class=\"appScopes svelte-10ho5mo\">");
    const cJSON *s;
    cJSON_ArrayForEach(s, cJSON_GetObjectItemCaseSensitive(a, "scopes")) {
        if (!cJSON_IsString(s)) continue;
        char key[96];
        snprintf(key, sizeof key, "oauth.scope.%s", s->valuestring);
        sb_append(sb, "<li>");
        sb_append_esc(sb, I18N(key, s->valuestring));
        sb_append(sb, "</li>");
    }
    sb_appendf(sb, "</ul> <span class=\"appLink svelte-10ho5mo\" role=\"button\" tabindex=\"0\" onclick=\"knAppActivity('%s')\">", idq);
    sb_append_esc(sb, I18N("settings.apps.activity", "View activity"));
    sb_append(sb, "</span> ");
    const cJSON *act = cJSON_GetObjectItemCaseSensitive(g_apps.activity, id);
    if (act) {
        sb_append(sb, "<div class=\"appActivity svelte-10ho5mo\">");
        if (cJSON_IsNull(act)) {
            sb_append_esc(sb, I18N("generic.wait", "Please wait..."));
        } else if (!cJSON_GetArraySize(act)) {
            sb_append_esc(sb, I18N("settings.apps.activity.none", "No activity yet."));
        } else {
            const cJSON *r;
            cJSON_ArrayForEach(r, act) {
                char when[48];
                locale_time(jnum(r, "created_at"), 1, when, sizeof when);
                const char *res = jstr(r, "result");
                sb_append(sb, "<div class=\"actRow svelte-10ho5mo\"><span>");
                sb_append_esc(sb, when);
                sb_appendf(sb, "</span> <span%s>", res && !strcmp(res, "ok") ? "" : " class=\"actBad svelte-10ho5mo\"");
                sb_append_esc(sb, jstr(r, "action") ? jstr(r, "action") : "");
                sb_append(sb, "</span> ");
                if (jstr(r, "scope")) {
                    sb_append(sb, "<span class=\"actScope svelte-10ho5mo\">");
                    sb_append_esc(sb, jstr(r, "scope"));
                    sb_append(sb, "</span>");
                }
                sb_append(sb, "</div>");
            }
        }
        sb_append(sb, "</div>");
    }
    sb_append(sb, "</div> ");
    if (!g_apps.kio) { // Disconnect (a krunker.io account's apps are disconnected on krunker.io)
        const int ask = !strcmp(g_apps.confirm, id);
        char confirm[256];
        I18N_F(confirm, "settings.apps.revoke.confirm", "Disconnect {0}? It will lose access to your account immediately.", name);
        sb_appendf(sb, "<div class=\"lnkTw appRevoke svelte-10ho5mo%s\" role=\"button\" tabindex=\"0\" onclick=\"knAppRevoke('%s')\">",
                   ask ? " appConfirm" : "", idq);
        sb_append_esc(sb, ask ? confirm : I18N("settings.apps.revoke", "Disconnect"));
        sb_append(sb, "</div>");
    }
    sb_append(sb, "</div>");
}

// the component's markup (:97561-97673), into the mount
void account_links_apps_html(StrBuf *sb, const int krunker_io) {
    if (g_apps.kio != krunker_io) { // the other account's list
        cJSON_Delete(g_apps.list);
        g_apps.list = NULL;
        g_apps.kio = krunker_io;
    }
    if (!g_apps.list || glfwGetTime() - g_apps.at > STATUS_TTL_S) apps_load();
    sb_append(sb, "<div class=\"appsInfo svelte-10ho5mo\">");
    sb_append_esc(sb, krunker_io ? I18N("settings.apps.desc", "Apps you signed in to with your Krunker account.")
                                 : I18N("kn.settings.apps.desc", "Apps you signed in to with your Box Office account."));
    sb_append(sb, "</div> ");
    if (!g_apps.list) {
        sb_append(sb, "<div class=\"appsEmpty svelte-10ho5mo\">");
        sb_append_esc(sb, I18N("generic.wait", "Please wait..."));
        sb_append(sb, "</div>");
    } else if (!cJSON_GetArraySize(g_apps.list)) {
        sb_append(sb, "<div class=\"appsEmpty svelte-10ho5mo\">");
        sb_append_esc(sb, I18N("settings.apps.none", "You haven't connected any apps."));
        sb_append(sb, "</div>");
    } else {
        const cJSON *a;
        cJSON_ArrayForEach(a, g_apps.list) app_html(sb, a);
    }
    if (krunker_io) {
        sb_append(sb, " <div class=\"appsInfo svelte-10ho5mo\">");
        sb_append_esc(sb, I18N("kn.settings.apps.kioReadOnly", "Disconnect these apps on krunker.io."));
        sb_append(sb, "</div>");
    }
}

static void apps_redraw(void) {
    const HtmlElem m = html_dom_by_id("connectedAppsMount");
    if (!m) return;
    StrBuf sb = {0};
    const int was = g_apps.loading;
    g_apps.loading = 1;
    account_links_apps_html(&sb, g_apps.kio);
    g_apps.loading = was;
    char *html = sb_finish(&sb);
    html_dom_set_html(m, html);
    free(html);
}

static void revoke_done(void *user, const int status, const char *body, const size_t len) {
    (void) status;
    (void) body;
    (void) len;
    if ((unsigned) (uintptr_t) user != g_dc.gen) return;
    g_apps.busy = 0;
    g_apps.loading = 0;
    g_apps.at = 0;
    apps_load(); // v10: revokeApp, then connectedApps again
    apps_redraw();
}

static void revoke_confirm_off(void *user) {
    (void) user;
    g_apps.confirm_timer = 0;
    g_apps.confirm[0] = 0;
    apps_redraw();
}

typedef struct {
    unsigned gen;
    char id[96];
} ActCtx;

static void activity_set(const char *id, cJSON *rows) {
    if (!g_apps.activity) g_apps.activity = cJSON_CreateObject();
    cJSON_DeleteItemFromObjectCaseSensitive(g_apps.activity, id);
    cJSON_AddItemToObject(g_apps.activity, id, rows);
}

static void activity_brain_done(void *user, const int status, const char *body, const size_t len) {
    ActCtx *c = user;
    if (c->gen == g_dc.gen && cJSON_HasObjectItem(g_apps.activity, c->id)) {
        cJSON *j = status == 200 && body ? cJSON_ParseWithLength(body, len) : NULL;
        cJSON *d = cJSON_DetachItemFromObjectCaseSensitive(j, "data");
        activity_set(c->id, cJSON_IsArray(d) ? d : (cJSON_Delete(d), cJSON_CreateArray())); // appActivity().data ?? []
        cJSON_Delete(j);
        apps_redraw();
    }
    free(c);
}

static void activity_kio_done(void *user, const int status, const cJSON *res) {
    ActCtx *c = user;
    if (c->gen == g_dc.gen && cJSON_HasObjectItem(g_apps.activity, c->id)) {
        const cJSON *d = status == 200 ? cJSON_GetObjectItemCaseSensitive(res, "data") : NULL;
        activity_set(c->id, cJSON_IsArray(d) ? cJSON_Duplicate(d, 1) : cJSON_CreateArray());
        apps_redraw();
    }
    free(c);
}

static const char *arg_str(const HtmlArg *args, const int argc) {
    return argc > 0 && args[0].kind == HTML_ARG_STR ? args[0].str : NULL;
}

int account_links_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    if (!strcmp(name, "knDiscordLink")) { // :97349-97358 (no returnTo: the browser page says the result)
        if (g_dc.busy || g_dc.waiting) return 1;
        g_dc.busy = 1;
        g_dc.result[0] = 0;
        dc_post("/discord/link", "{}", dc_link_done);
        dc_redraw();
        return 1;
    }
    if (!strcmp(name, "knDiscordCancel")) { // :97321-97333: stop waiting, read the status again
        dc_stop_wait();
        dc_load();
        dc_redraw();
        return 1;
    }
    if (!strcmp(name, "knDiscordJoin")) { // :97359-97369
        if (g_dc.busy) return 1;
        g_dc.busy = 1;
        g_dc.result[0] = 0;
        dc_post("/discord/join", NULL, dc_join_done);
        dc_redraw();
        return 1;
    }
    if (!strcmp(name, "knDiscordUnlink")) { // :97371-97386: once more within 3 s
        if (g_dc.busy) return 1;
        if (!g_dc.confirm) {
            g_dc.confirm = 1;
            if (g_dc.confirm_timer) html_timer_cancel(g_dc.confirm_timer);
            g_dc.confirm_timer = html_timer_add(UNLINK_CONFIRM_MS, dc_confirm_off, NULL);
            dc_redraw();
            return 1;
        }
        if (g_dc.confirm_timer) html_timer_cancel(g_dc.confirm_timer);
        g_dc.confirm_timer = 0;
        g_dc.confirm = 0;
        g_dc.busy = 1;
        g_dc.result[0] = 0;
        dc_post("/discord/unlink", NULL, dc_unlink_done);
        dc_redraw();
        return 1;
    }
    if (!strcmp(name, "knAppRevoke")) { // :97529-97541: once more within 4 s
        const char *id = arg_str(args, argc);
        if (!id || g_apps.busy || g_apps.kio || !app_by_id(id)) return 1;
        if (strcmp(g_apps.confirm, id)) {
            copy_str(g_apps.confirm, sizeof g_apps.confirm, id);
            if (g_apps.confirm_timer) html_timer_cancel(g_apps.confirm_timer);
            g_apps.confirm_timer = html_timer_add(REVOKE_CONFIRM_MS, revoke_confirm_off, NULL);
            apps_redraw();
            return 1;
        }
        if (g_apps.confirm_timer) html_timer_cancel(g_apps.confirm_timer);
        g_apps.confirm_timer = 0;
        g_apps.confirm[0] = 0;
        char enc[200], path[320], url[1024];
        backend_path_segment(enc, sizeof enc, id);
        snprintf(path, sizeof path, "/account/apps/%s", enc);
        g_apps.busy = 1;
        if (!session_path(url, sizeof url, path) || !backend_http_request("DELETE", url, NULL, revoke_done, (void *) (uintptr_t) g_dc.gen))
            g_apps.busy = 0;
        apps_redraw();
        return 1;
    }
    if (!strcmp(name, "knAppActivity")) { // :97542-97558: a second click hides it
        const char *id = arg_str(args, argc);
        if (!id || !app_by_id(id)) return 1;
        if (cJSON_HasObjectItem(g_apps.activity, id)) {
            cJSON_DeleteItemFromObjectCaseSensitive(g_apps.activity, id);
            apps_redraw();
            return 1;
        }
        activity_set(id, cJSON_CreateNull());
        ActCtx *c = calloc(1, sizeof *c);
        if (c) {
            c->gen = g_dc.gen;
            copy_str(c->id, sizeof c->id, id);
            char enc[200], path[320], url[1024];
            backend_path_segment(enc, sizeof enc, id);
            snprintf(path, sizeof path, "/account/apps/%s/activity", enc);
            const int sent = g_apps.kio ? krunker_account_api("GET", KRUNKER_SVC_API, path, NULL, activity_kio_done, c)
                                        : session_path(url, sizeof url, path) && backend_http_request("GET", url, NULL, activity_brain_done, c);
            if (!sent) {
                free(c);
                activity_set(id, cJSON_CreateArray());
            }
        }
        apps_redraw();
        return 1;
    }
    return 0;
}

void account_links_social(const char *label) {
    if (label && !strcmp(label, "discordStatus")) dc_load(); // discord.c: the browser link finished
}

void account_links_reset(void) {
    dc_stop_wait();
    if (g_dc.confirm_timer) html_timer_cancel(g_dc.confirm_timer);
    if (g_apps.confirm_timer) html_timer_cancel(g_apps.confirm_timer);
    cJSON_Delete(g_dc.status);
    cJSON_Delete(g_apps.list);
    cJSON_Delete(g_apps.activity);
    const unsigned gen = g_dc.gen + 1;
    memset(&g_dc, 0, sizeof g_dc);
    memset(&g_apps, 0, sizeof g_apps);
    g_dc.gen = gen;
}
