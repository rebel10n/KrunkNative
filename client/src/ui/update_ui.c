// Client updates in the menu (client/include/update_ui.h).
#include <update_ui.h>
#include <app_mode.h>        // app_reload: the web's Update now
#include <console.h>
#include <game_reload.h>     // game_reload_update_available / _update_pending: the automatic restart
#include <ui_logic.h>

#include <stdio.h>
#include <string.h>

#define CHECK_DELAY_S 3.0
#define CHECK_INTERVAL_S 3600.0

// ------------------------------------------------------------------ the box (both builds)

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
// outdated: the "Outdated" colour; latest: the new build; this_build: the title's own version;
// update_now: offer the Update now link (a restart that runs the new build).
static void render_box(const int show, const int outdated, const char *latest, const char *this_build, const int update_now) {
    HtmlElem old = html_dom_by_id("knUpdateAd");
    if (old) html_dom_remove(old);
    if (!show) return;
    HtmlElem ref = html_dom_by_id("updateAd");
    if (!ref) return;
    char latest_esc[160], html[2304], title_text[256], title[512];
    esc(latest_esc, sizeof(latest_esc), latest);
    i18n_trf(title_text, sizeof(title_text), "kn.update.thisClient", "This client: {0}", 1, I18N_ARGV(this_build));
    esc(title, sizeof(title), title_text);
    char now_link[256] = "";
    if (update_now)
        snprintf(now_link, sizeof(now_link), "<a id='knUpdateNow' onclick='knUpdateNow()' style='cursor:pointer;color:#fbc02d;text-decoration:underline;margin-right:12px'>%s</a>",
                 I18N("kn.update.now", "Update now"));
    // the automatic restart's state (game_reload.c): waiting for the match to end, or due
    char auto_line[256] = "";
    const int pending = game_reload_update_pending();
    if (pending)
        snprintf(auto_line, sizeof(auto_line), "<div id='knUpdateAuto' style='margin-top:4px;font-size:14px;color:rgba(255,255,255,0.7)'>%s</div>",
                 pending == 2 ? I18N("kn.update.restarting", "Restarting...") : I18N("kn.update.restartAfterMatch", "Restarts after this match"));
    snprintf(html, sizeof(html),
             "<div class='youNewDiv' id='knUpdateAd' style='display:block;cursor:default' title='%s'>"
             "<span class='material-icons' style='vertical-align:middle;font-size:30px;color:%s'>system_update</span>"
             "<div class='helpTxtHol' style='margin-left:6px'>"
             "<span id='knUpdateState' style='color:%s'>%s</span>"
             "<div id='knUpdateVersion' style='font-size:14px;color:rgba(255,255,255,0.7)'>%s</div>"
             "<div style='margin-top:4px;font-size:15px'>"
             "%s"
             "<a id='knUpdateDownload' onclick='knUpdateDownload()' style='cursor:pointer;color:#fff;text-decoration:underline'>%s</a>"
             "</div>%s</div></div>",
             title, outdated ? "#fb8c00" : "#fbc02d", outdated ? "#fb8c00" : "#fbc02d",
             outdated ? I18N("kn.update.outdated", "Outdated") : I18N("kn.update.available", "Update available"), latest_esc,
             now_link, I18N("kn.update.download", "Download"), auto_line);
    html_dom_insert_html(ref, HTML_INSERT_BEFORE_BEGIN, html);
}

#ifdef __EMSCRIPTEN__
// ------------------------------------------------------------------ web: the page's build stamp
//
// The web build is served from the site itself: a newer client is a newer page. deploy/pages/
// stage.py stamps every deployed page with <meta name="kn-build" content="<hash16>"> (the content
// hash of the client files it loads) and names the immutable client.<hash>.* files after it. The
// client fetches its own page again (no cache, same origin) and compares the stamps: a different
// one means a deploy landed since this tab loaded, and a reload runs it. The plain build (make
// web) has an empty stamp: nothing to compare.
#include <emscripten.h>
#include <build_info.h>
#include <stdlib.h>

// 1 = a check started (Module.knPageBuild follows it), 0 = this page has no stamp.
EM_JS(int, kn_js_page_build_check, (void), {
    var cur = "";
    try {
        var t = document.querySelector('meta[name="kn-build"]');
        cur = t ? t.getAttribute("content") || "" : "";
    } catch (e) {}
    if (!cur) return 0;
    Module.knPageBuild = {state: 1, cur: cur, live: ""};
    var url = "";
    try { url = location.pathname || "/"; } catch (e) { url = "/"; }
    fetch(url, {cache: "no-store", credentials: "same-origin"}).then(function (r) {
        if (!r.ok) throw new Error("HTTP " + r.status);
        return r.text();
    }).then(function (html) {
        var t = new DOMParser().parseFromString(html, "text/html").querySelector('meta[name="kn-build"]');
        Module.knPageBuild = {state: 2, cur: cur, live: t ? t.getAttribute("content") || "" : ""};
    }).catch(function (e) {
        Module.knPageBuild = {state: 3, cur: cur, live: "", error: String(e && e.message ? e.message : e)};
    });
    return 1;
});
EM_JS(int, kn_js_page_build_state, (void), { var b = Module.knPageBuild; return b ? b.state : 0; }); // 1 running, 2 done, 3 failed
EM_JS(char *, kn_js_page_build_value, (int live), {
    var b = Module.knPageBuild;
    return stringToNewUTF8(b ? (live ? b.live : b.cur) || "" : "");
});
EM_JS(char *, kn_js_page_build_error, (void), { var b = Module.knPageBuild; return stringToNewUTF8(b && b.error ? b.error : ""); });

static struct {
    int enabled;       // the page has a build stamp
    int local;         // a local build (kn_build_info()->local): no box for a newer page
    int init;
    double next_check; // glfwGetTime of the next check
    int running;
    int checked;       // one check finished
    int newer;         // the live page's stamp differs
    char live[40];
    Client *client;
} g_web;

void update_ui_tick(Client *client) {
    g_web.client = client;
    if (!g_web.init) {
        g_web.init = 1;
        // the first fetch tells whether the page is stamped; its result is this page's own stamp
        // (the browser fetched the page a moment ago), so it is not compared
        g_web.enabled = kn_js_page_build_check();
        g_web.running = g_web.enabled;
        g_web.local = kn_build_info()->local;
        g_web.next_check = glfwGetTime() + CHECK_INTERVAL_S;
        if (!g_web.enabled) printf("[update] the web client is served by the site: no build stamp on this page, no checks\n");
        return;
    }
    if (!g_web.enabled) return;
    const double now = glfwGetTime();
    if (g_web.running) {
        const int state = kn_js_page_build_state();
        if (state < 2) return;
        g_web.running = 0;
        if (state == 3) {
            char *err = kn_js_page_build_error();
            printf("[update] page build check failed: %s\n", err ? err : "?");
            free(err);
            return;
        }
        char *live = kn_js_page_build_value(1), *cur = kn_js_page_build_value(0);
        snprintf(g_web.live, sizeof(g_web.live), "%s", live ? live : "");
        const int newer = g_web.checked && live && live[0] && cur && cur[0] && strcmp(live, cur) != 0;
        if (g_web.checked) printf("[update] page build %s, live %s: %s\n", cur ? cur : "?", live && live[0] ? live : "?", newer ? "newer page deployed" : "current");
        free(live);
        free(cur);
        g_web.checked = 1;
        if (newer && !g_web.newer && !g_web.local) {
            g_web.newer = 1;
            render_box(1, 0, g_web.live, kn_build_info()->describe, 1);
            game_reload_update_available(g_web.live); // the automatic restart (Settings > Client)
        }
        return;
    }
    if (g_web.newer) return; // known: the box stays until the reload
    if (now >= g_web.next_check) {
        g_web.next_check = now + CHECK_INTERVAL_S;
        g_web.running = kn_js_page_build_check();
    }
}

int update_ui_restart_via_launcher(Client *client, char *const *args) {
    (void) client;
    (void) args;
    return 0;
}

int update_ui_newer_ready(void) {
    return g_web.newer;
}

int update_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    (void) args;
    (void) argc;
    if (!name) return -1;
    if (!strcmp(name, "knUpdateNow")) {
        printf("[update] update now: reloading the page\n");
        app_reload(NULL); // location.reload(): the address bar keeps the game
        return 1;
    }
    if (!strcmp(name, "knUpdateDownload")) {
        app_reload(NULL);
        return 1;
    }
    return -1;
}

void update_ui_console(const int argc, char **argv) {
    const char *sub = argc > 0 && argv[0] ? argv[0] : "";
    if (!strcmp(sub, "now")) {
        app_reload(NULL);
        return;
    }
    if (!strcmp(sub, "check")) {
        if (!g_web.enabled) {
            ui_console_printf(NULL, "this page has no build stamp (a plain web build): the site serves the latest client");
            return;
        }
        g_web.running = kn_js_page_build_check();
        ui_console_printf(NULL, "checking the page's build stamp ...");
        return;
    }
    if (!g_web.enabled) ui_console_printf(NULL, "The web client is always the latest version (no build stamp on this page).");
    else ui_console_printf(NULL, "this page: %s | live: %s | %s", kn_build_info()->describe, g_web.live[0] ? g_web.live : "(not checked yet)",
                           g_web.newer ? "a newer page is deployed: `update now` reloads" : "current");
}

void update_ui_scenario(const cJSON *update) { (void) update; }

#else
// ------------------------------------------------------------------ native: the updates manifest

#include <build_info.h>
#include <headless.h>
#include <html_ui.h>
#include <kn_fs.h>
#include <kn_http.h>
#include <kn_install.h>
#include <kn_update.h>

#include <pthread.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#define MANIFEST_MAX (4u * 1024u * 1024u)
// The build a restart through the launcher was for (its commit): the launcher and the client it
// starts inherit it. A client that comes back still older than that build (the launcher could
// not install it: the staged client did not run here, a read-only install, a failed download)
// does not restart for it again; the box keeps Update now / Download.
#define UPDATE_TRIED_ENV "KRUNKNATIVE_UPDATE_TRIED"

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
    int announced;     // game_reload_update_available told once for this build
    // a local build (a dirty tree, a commit origin/main does not have: kn_build_info()->local) is
    // never the published one: its state stays CURRENT, so no box, no Outdated, no automatic restart.
    // KRUNKNATIVE_LOCAL_BUILD=1 / =0 overrides the build's flag (the e2e tests pin it).
    int local;
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
    const char *local_env = getenv("KRUNKNATIVE_LOCAL_BUILD");
    g_up.local = local_env && *local_env ? *local_env == '1' : kn_build_info()->local;
    if (g_up.local) printf("[update] this build is local (not published): no update box, no Outdated\n");
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
        g_up.state = g_up.local ? KN_UPDATE_CURRENT : kn_update_compare(&mine, &m.build, m.grace_s);
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

static void render(void);
static void render_on_locale(void *user) {
    (void) user;
    render();
}

static void render(void) {
    static int listening;
    if (!listening) { // the box is written once per check: again in a newly picked language
        listening = 1;
        i18n_on_change(render_on_locale, NULL);
    }
    render_box(g_up.ok && g_up.state != KN_UPDATE_CURRENT, g_up.state == KN_UPDATE_OUTDATED, g_up.latest, kn_build_info()->describe,
               can_update_now());
}

static void report_console(void) {
    if (!g_up.ok) {
        ui_console_printf("#ff6b6b", "update check failed: %s", g_up.error[0] ? g_up.error : "unknown error");
        return;
    }
    if (g_up.local) {
        ui_console_printf(NULL, "this client: %s | latest: %s (%.12s) | a local build (not published): no update", kn_build_string(),
                          g_up.latest, g_up.latest_commit);
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
    if (fresh) {
        if (g_up.ok) printf("[update] latest %s (%.12s): %s%s\n", g_up.latest, g_up.latest_commit,
                            g_up.local ? "local build, no update" : kn_update_state_name(g_up.state),
                            g_up.state != KN_UPDATE_CURRENT && can_update_now() ? " (Update now: via the launcher)" : "");
        else printf("[update] check failed: %s\n", g_up.error);
        fflush(stdout);
        // a newer build the launcher can install: the automatic restart (Settings > Client), once
        if (g_up.ok && g_up.state != KN_UPDATE_CURRENT && can_update_now() && !g_up.announced) {
            g_up.announced = 1;
            const char *tried = getenv(UPDATE_TRIED_ENV);
            if (tried && *tried && kn_update_same_commit(tried, g_up.latest_commit))
                printf("[update] %s was tried already and is not installed (see the launcher's log): no automatic restart for it\n", g_up.latest);
            else
                game_reload_update_available(g_up.latest);
        }
        render();
        if (g_up.console_report) {
            g_up.console_report = 0;
            report_console();
        }
    }
    // the box follows the automatic restart's state (waiting for the match / due)
    static int shown_pending = -1;
    const int pending = game_reload_update_pending();
    if (pending != shown_pending) {
        shown_pending = pending;
        if (g_up.ok && g_up.state != KN_UPDATE_CURRENT) render();
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

int update_ui_restart_via_launcher(Client *client, char *const *args) {
    init_once();
    if (!g_up.launcher[0]) return 0;
    char pid[32];
#ifdef _WIN32
    snprintf(pid, sizeof(pid), "%lu", (unsigned long) GetCurrentProcessId());
#else
    snprintf(pid, sizeof(pid), "%ld", (long) getpid());
#endif
    // krunknative --wait-pid <pid> -- <the client's command line>: the launcher's own options came
    // from its environment too (KRUNKNATIVE_UPDATES_URL, KRUNKNATIVE_ASSETS_DIR), which it inherits
    int n = 0;
    while (args && args[0] && args[n + 1]) n++;
    char **argv = calloc((size_t) n + 6, sizeof(char *));
    if (!argv) return 0;
    int k = 0;
    argv[k++] = g_up.launcher;
    argv[k++] = "--wait-pid";
    argv[k++] = pid;
    if (n) argv[k++] = "--";
    for (int i = 0; i < n; i++) argv[k++] = args[i + 1];
    argv[k] = NULL;
    const int tried = g_up.ok && g_up.state != KN_UPDATE_CURRENT && g_up.latest_commit[0];
#ifdef _WIN32
    if (tried) _putenv_s(UPDATE_TRIED_ENV, g_up.latest_commit);
#else
    if (tried) setenv(UPDATE_TRIED_ENV, g_up.latest_commit, 1);
#endif
    printf("[update] restarting through %s (pid %s waits)\n", g_up.launcher, pid);
    fflush(stdout);
#ifndef _WIN32
    // like app_reload: sockets, files and the GL/X connection stay out of the launcher (which
    // execs the new client): close them on exec only, so a failed spawn leaves this process intact
    for (int fd = 3; fd < 1024; fd++) {
        const int flags = fcntl(fd, F_GETFD);
        if (flags >= 0) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
#endif
    const int ok = kn_proc_spawn_detached(g_up.launcher, argv, NULL);
    free(argv);
    if (!ok) {
        fprintf(stderr, "[update] could not start the launcher %s\n", g_up.launcher);
#ifdef _WIN32
        if (tried) _putenv_s(UPDATE_TRIED_ENV, "");
#else
        if (tried) unsetenv(UPDATE_TRIED_ENV);
#endif
        return 0;
    }
    if (client && client->window) glfwSetWindowShouldClose(client->window, GLFW_TRUE);
    return 1;
}

int update_ui_newer_ready(void) {
    return g_up.ok && g_up.state != KN_UPDATE_CURRENT && can_update_now();
}

// Update now: restart through the launcher (it waits for this process, updates, starts the new
// client with this command line); without one, the download.
static int update_now(void) {
    if (!can_update_now()) {
        open_download();
        return 0;
    }
    app_reload_prepare(); // the window state for the new process
    if (!update_ui_restart_via_launcher(g_up.client, app_exec_args())) {
        ui_alert(I18N("kn.update.launcherFailed", "Could not start the launcher: download the update instead."));
        return 0;
    }
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
        if (g_up.local) {
            ui_console_printf(NULL, "a local build is not published: there is no update to install");
            return;
        }
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
