// Client updates in the menu (client/include/update_ui.h).
#include <update_ui.h>
#include <console.h>
#include <ui_logic.h>

#ifdef __EMSCRIPTEN__
// The web build is served from the site itself: always the latest.
void update_ui_tick(Client *client) { (void) client; }
int update_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) name; (void) ev; (void) args; (void) argc;
    return -1;
}
void update_ui_console(const int argc, char **argv) {
    (void) argc; (void) argv;
    ui_console_printf(NULL, "The web client is always the latest version.");
}
void update_ui_scenario(const cJSON *update) { (void) update; }
#else

#include <build_info.h>
#include <headless.h>
#include <html_ui.h>
#include <kn_fs.h>
#include <kn_http.h>
#include <kn_install.h>
#include <kn_update.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#define CHECK_DELAY_S 3.0
#define CHECK_INTERVAL_S 3600.0
#define MANIFEST_MAX (4u * 1024u * 1024u)

typedef enum { CHK_IDLE, CHK_RUNNING, CHK_DONE } CheckPhase;

typedef struct {
    pthread_mutex_t lock;
    int init, enabled;
    char url[KN_UPDATE_URL_MAX];
    double next_check; // glfwGetTime of the next automatic check (0 = after CHECK_DELAY_S)
    CheckPhase phase;
    int fresh;         // a result the page has not shown yet
    int ok;            // the last check succeeded
    char error[256];
    KnUpdateState state;
    char latest[64], latest_commit[48];
    char download[KN_UPDATE_URL_MAX];
    int scenario;      // --render-scenario stand-in
    int console_report; // print the next result to the console
    // install facts (kn-install.json in the cwd, the launcher's env)
    KnInstallMarker marker;
    int has_marker;
    char launcher[KN_PATH_MAX];
    Client *client;
} UpdateUi;

static UpdateUi g_up = {.lock = PTHREAD_MUTEX_INITIALIZER};

static int can_update_now(void) {
    return g_up.launcher[0] && g_up.has_marker && kn_install_self_updatable(&g_up.marker);
}

static void self_build(KnUpdateBuild *b) {
    const KnBuildInfo *bi = kn_build_info();
    memset(b, 0, sizeof(*b));
    snprintf(b->describe, sizeof(b->describe), "%s", bi->describe);
    snprintf(b->commit, sizeof(b->commit), "%s", bi->commit);
    b->commit_time = bi->commit_time;
    b->proto_major = bi->protocol_major;
    b->proto_minor = bi->protocol_minor;
}

static void init_once(void) {
    if (g_up.init) return;
    g_up.init = 1;
    const char *env = getenv("KRUNKNATIVE_UPDATES_URL");
    snprintf(g_up.url, sizeof(g_up.url), "%s", env && *env ? env : KN_UPDATE_DEFAULT_URL);
    // headless runs (tests, captures, smoke) never reach the production server unless asked
    g_up.enabled = strcmp(g_up.url, "off") != 0 && !ui_scenario_active() && (!headless_enabled() || (env && *env));
    char *json = NULL;
    size_t len = 0;
    if (kn_fs_read(KN_INSTALL_MARKER, &json, &len, 65536)) g_up.has_marker = kn_install_marker_parse(json, len, &g_up.marker);
    free(json);
    const char *launcher = getenv("KRUNKNATIVE_LAUNCHER");
    if (launcher && *launcher && kn_fs_exists(launcher)) snprintf(g_up.launcher, sizeof(g_up.launcher), "%s", launcher);
    if (g_up.enabled) printf("[update] checking %s (this build %s)\n", g_up.url, kn_build_string());
}

static void *check_thread(void *arg) {
    (void) arg;
    char url[KN_UPDATE_URL_MAX];
    pthread_mutex_lock(&g_up.lock);
    snprintf(url, sizeof(url), "%s", g_up.url);
    pthread_mutex_unlock(&g_up.lock);
    char ua[128];
    snprintf(ua, sizeof(ua), "KrunkNative/%s (%s)", kn_build_info()->describe, kn_update_platform_default());
    kn_http_set_user_agent(ua);
    KnHttp *http = kn_http_new(10000);
    char *body = NULL, err[256] = "";
    size_t len = 0;
    const int status = http ? kn_http_get_mem(http, url, MANIFEST_MAX, &body, &len, err, sizeof(err)) : 0;
    kn_http_free(http);
    KnUpdateManifest m;
    const int ok = status == 200 && kn_update_manifest_parse(body, len, url, &m, err, sizeof(err));
    free(body);
    pthread_mutex_lock(&g_up.lock);
    g_up.ok = ok;
    if (ok) {
        KnUpdateBuild mine;
        self_build(&mine);
        g_up.state = kn_update_compare(&mine, &m.build, m.grace_s);
        snprintf(g_up.latest, sizeof(g_up.latest), "%s", m.build.describe);
        snprintf(g_up.latest_commit, sizeof(g_up.latest_commit), "%s", m.build.commit);
        const char *platform = g_up.has_marker && g_up.marker.platform[0] ? g_up.marker.platform : kn_update_platform_default();
        const char *kind = g_up.has_marker && g_up.marker.package[0] && strcmp(g_up.marker.package, "app") != 0 ? g_up.marker.package : NULL;
        snprintf(g_up.download, sizeof(g_up.download), "%s", kn_update_download_url(&m, platform, kind));
        g_up.error[0] = 0;
        kn_update_manifest_free(&m);
    } else {
        snprintf(g_up.error, sizeof(g_up.error), "%s", status && status != 200 ? "HTTP error" : err);
        if (status && status != 200) snprintf(g_up.error, sizeof(g_up.error), "HTTP %d", status);
    }
    g_up.phase = CHK_DONE;
    g_up.fresh = 1;
    pthread_mutex_unlock(&g_up.lock);
    return NULL;
}

static void start_check(void) {
    pthread_mutex_lock(&g_up.lock);
    const int busy = g_up.phase == CHK_RUNNING;
    if (!busy) g_up.phase = CHK_RUNNING;
    pthread_mutex_unlock(&g_up.lock);
    if (busy) return;
    pthread_t t;
    if (pthread_create(&t, NULL, check_thread, NULL) == 0) {
        pthread_detach(t);
    } else {
        pthread_mutex_lock(&g_up.lock);
        g_up.phase = CHK_IDLE;
        pthread_mutex_unlock(&g_up.lock);
    }
}

// ------------------------------------------------------------------ page

static void esc(char *out, const size_t n, const char *s) {
    size_t o = 0;
    for (; s && *s && o + 7 < n; s++) {
        const char *rep = *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '&' ? "&amp;" : *s == '"' ? "&quot;" : *s == '\'' ? "&#39;" : NULL;
        if (rep) {
            const size_t rl = strlen(rep);
            memcpy(out + o, rep, rl);
            o += rl;
        } else {
            out[o++] = *s;
        }
    }
    out[o] = 0;
}

// The box, a youNewDiv like the menu's other notices, right before v7's "Whats new ?" (#updateAd).
static void render(void) {
    HtmlElem old = html_dom_by_id("knUpdateAd");
    if (old) html_dom_remove(old);
    if (g_up.state == KN_UPDATE_CURRENT || !g_up.ok) return;
    HtmlElem ref = html_dom_by_id("updateAd");
    if (!ref) return;
    const int outdated = g_up.state == KN_UPDATE_OUTDATED;
    char latest[160], mine[160], html[2048];
    esc(latest, sizeof(latest), g_up.latest);
    esc(mine, sizeof(mine), kn_build_info()->describe);
    snprintf(html, sizeof(html),
             "<div class='youNewDiv' id='knUpdateAd' style='display:block;cursor:default' title='This client: %s'>"
             "<span class='material-icons' style='vertical-align:middle;font-size:30px;color:%s'>system_update</span>"
             "<div class='helpTxtHol' style='margin-left:6px'>"
             "<span id='knUpdateState' style='color:%s'>%s</span>"
             "<div id='knUpdateVersion' style='font-size:14px;color:rgba(255,255,255,0.7)'>%s</div>"
             "<div style='margin-top:4px;font-size:15px'>"
             "%s"
             "<a id='knUpdateDownload' onclick='knUpdateDownload()' style='cursor:pointer;color:#fff;text-decoration:underline'>Download</a>"
             "</div></div></div>",
             mine, outdated ? "#fb8c00" : "#fbc02d", outdated ? "#fb8c00" : "#fbc02d", outdated ? "Outdated" : "Update available", latest,
             can_update_now() ? "<a id='knUpdateNow' onclick='knUpdateNow()' style='cursor:pointer;color:#fbc02d;text-decoration:underline;margin-right:12px'>Update now</a>" : "");
    html_dom_insert_html(ref, HTML_INSERT_BEFORE_BEGIN, html);
}

static void report_console(void) {
    if (!g_up.ok) {
        ui_console_printf("#ff6b6b", "update check failed: %s", g_up.error[0] ? g_up.error : "unknown error");
        return;
    }
    ui_console_printf(NULL, "this client: %s | latest: %s (%.12s) | %s", kn_build_string(), g_up.latest, g_up.latest_commit,
                      g_up.state == KN_UPDATE_OUTDATED ? "OUTDATED" : g_up.state == KN_UPDATE_AVAILABLE ? "update available" : "up to date");
    if (g_up.state != KN_UPDATE_CURRENT)
        ui_console_printf(NULL, can_update_now() ? "run `update now` to update through the launcher, or `update download`"
                                                 : "run `update download` for the new version");
}

void update_ui_tick(Client *client) {
    init_once();
    g_up.client = client;
    if (g_up.scenario) return;
    const double now = glfwGetTime();
    if (g_up.enabled && g_up.phase != CHK_RUNNING) {
        if (!g_up.next_check) g_up.next_check = now + CHECK_DELAY_S;
        if (now >= g_up.next_check) {
            g_up.next_check = now + CHECK_INTERVAL_S;
            start_check();
        }
    }
    pthread_mutex_lock(&g_up.lock);
    const int fresh = g_up.fresh;
    g_up.fresh = 0;
    pthread_mutex_unlock(&g_up.lock);
    if (!fresh) return;
    if (g_up.ok) printf("[update] latest %s (%.12s): %s%s\n", g_up.latest, g_up.latest_commit, kn_update_state_name(g_up.state),
                        g_up.state != KN_UPDATE_CURRENT && can_update_now() ? " (Update now: via the launcher)" : "");
    else printf("[update] check failed: %s\n", g_up.error);
    fflush(stdout);
    render();
    if (g_up.console_report) {
        g_up.console_report = 0;
        report_console();
    }
}

// ------------------------------------------------------------------ actions

static int download_url_ok(const char *url) {
    // the updates store: dl.krunk.io (kru), formerly assets.krunk.io (now the Pages CDN, which 302s /updates/ there)
    return !strncmp(url, "https://github.com/rebel10n/KrunkNative/", 39) || !strncmp(url, "https://dl.krunk.io/updates/", 28) ||
           !strncmp(url, "https://assets.krunk.io/updates/", 32) ||
           (getenv("KRUNKNATIVE_UPDATES_URL") && !strncmp(url, "http", 4));
}

static void open_download(void) {
    const char *url = g_up.download[0] && download_url_ok(g_up.download) ? g_up.download : "https://github.com/rebel10n/KrunkNative/releases/tag/client-latest";
    ui_open_external(url);
}

// Restart through the launcher: it waits for this process, updates, starts the new client.
static int update_now(void) {
    if (!can_update_now()) {
        open_download();
        return 0;
    }
    char pid[32];
#ifdef _WIN32
    snprintf(pid, sizeof(pid), "%lu", (unsigned long) GetCurrentProcessId());
#else
    snprintf(pid, sizeof(pid), "%ld", (long) getpid());
#endif
    char *argv[] = {g_up.launcher, "--wait-pid", pid, NULL};
    printf("[update] restarting through %s\n", g_up.launcher);
    fflush(stdout);
    if (!kn_proc_spawn_detached(g_up.launcher, argv, NULL)) {
        ui_alert("Could not start the launcher: download the update instead.");
        return 0;
    }
    if (g_up.client && g_up.client->window) glfwSetWindowShouldClose(g_up.client->window, GLFW_TRUE);
    return 1;
}

int update_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    (void) args;
    (void) argc;
    if (!name) return -1;
    if (!strcmp(name, "knUpdateNow")) {
        update_now();
        return 1;
    }
    if (!strcmp(name, "knUpdateDownload")) {
        open_download();
        return 1;
    }
    return -1;
}

void update_ui_console(const int argc, char **argv) {
    init_once();
    const char *sub = argc > 0 && argv[0] ? argv[0] : "";
    if (!strcmp(sub, "check")) {
        if (!strcmp(g_up.url, "off")) {
            ui_console_printf("#ff6b6b", "update checks are off (KRUNKNATIVE_UPDATES_URL=off)");
            return;
        }
        g_up.console_report = 1;
        ui_console_printf(NULL, "checking %s ...", g_up.url);
        start_check();
        return;
    }
    if (!strcmp(sub, "download")) {
        open_download();
        return;
    }
    if (!strcmp(sub, "now")) {
        if (!g_up.ok || g_up.state == KN_UPDATE_CURRENT) {
            ui_console_printf(NULL, g_up.ok ? "already up to date" : "no update known yet: run `update check` first");
            return;
        }
        if (!can_update_now()) {
            ui_console_printf(NULL, g_up.has_marker && !kn_install_self_updatable(&g_up.marker)
                                        ? "installed through the package manager: update it there (or `update download`)"
                                        : "not started through the launcher: `update download` for the new version");
            return;
        }
        if (!html_ui_is_menu_open()) { // never in the middle of a round
            ui_console_printf(NULL, "open the menu (Esc), then run `update now`");
            return;
        }
        update_now();
        return;
    }
    if (!g_up.ok && !g_up.error[0]) {
        ui_console_printf(NULL, "this client: %s | no update check yet (`update check`)", kn_build_string());
        return;
    }
    report_console();
}

void update_ui_scenario(const cJSON *update) {
    if (!cJSON_IsObject(update)) return;
    init_once();
    g_up.scenario = 1;
    const char *state = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(update, "state"));
    const char *latest = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(update, "latest"));
    g_up.ok = 1;
    g_up.state = state && !strcmp(state, "available") ? KN_UPDATE_AVAILABLE : KN_UPDATE_OUTDATED;
    snprintf(g_up.latest, sizeof(g_up.latest), "%s", latest ? latest : "v1.0.0");
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(update, "launcher"))) {
        snprintf(g_up.launcher, sizeof(g_up.launcher), "krunknative");
        g_up.has_marker = 1;
        snprintf(g_up.marker.package, sizeof(g_up.marker.package), "tar.gz");
    }
    render();
}
#endif
