// KrunkNative launcher (doc/backend.md "Client updates"): what users start. Before the game it
//   1. checks the updates manifest (KN_UPDATE_DEFAULT_URL, --updates-url, KRUNKNATIVE_UPDATES_URL),
//   2. installs a newer executable-only package (sha256-verified; launcher, client_bin,
//      server_bin, libs and ui/ swapped with a journal; deb/rpm installs defer to the package
//      manager), never touching the game content,
//   3. syncs the game content when the manifest's assets version differs from the local one
//      (first install = everything, later only the files whose hash changed),
// then starts client_bin with KRUNKNATIVE_ASSETS_DIR and KRUNKNATIVE_LAUNCHER set, in the
// directory that holds ui/ (POSIX: exec, same pid; Windows: a new process).
//
//   krunknative [--cli|--headless] [--no-update] [--check] [--no-launch] [--updates-url URL]
//               [--assets-dir DIR] [--allow-checkout-assets] [--threads N] [--asset-mirrors URLS|off]
//               [--] [client args...]
// Unknown arguments go to client_bin. Exit codes: 0 ok, 1 usage, 2 failed with nothing
// playable, 3 --check found a newer build.
#include "launcher_ui.h"

#include <build_info.h>
#include <kn_archive.h>
#include <kn_assets.h>
#include <kn_fs.h>
#include <kn_http.h>
#include <kn_install.h>
#include <kn_update.h>

#include <auth_join.h>

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

#define MANIFEST_MAX (4u * 1024u * 1024u)
#define PACKAGE_MAX ((int64_t) 1 << 30)

typedef struct {
    // options
    int cli, no_update, check_only, no_launch;
    int allow_checkout_assets; // --allow-checkout-assets: sync into a git checkout's assets/ anyway
    long wait_pid; // the client's "Update now": let it exit first
    char updates_url[KN_UPDATE_URL_MAX];
    char assets_override[KN_PATH_MAX];
    char root_override[KN_PATH_MAX];
    char **client_args;
    int client_argc;
    // results (worker)
    KnInstall in;
    int have_install;
    char assets_dir[KN_PATH_MAX];
    KnUpdateState state;
    int manifest_ok;
    int updated;
    char latest[64];
    // shared view
    pthread_mutex_t lock;
    LauncherView view;
    int done, ok, can_play;
    volatile int cancel;
    char last_cli[400];
    double speed_t0;
    int64_t speed_b0;
    double speed;      // bytes/s, smoothed (EWMA over ~1 s samples); 0 = no sample yet
    double xfer_t0;    // when this transfer started (ETA needs a few seconds of history)
    int download_started;
    int threads;       // --threads / KRUNKNATIVE_ASSET_THREADS (0 = default)
    char mirrors[1024]; // --asset-mirrors / KRUNKNATIVE_ASSET_MIRRORS ("off" = none)
    char origins[4096]; // the sync's origins for the client's on-demand fetch (KRUNKNATIVE_ASSET_ORIGINS)
} Launcher;

static Launcher L;

static double now_s(void) {
#ifdef _WIN32
    return (double) GetTickCount64() / 1000.0;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
#endif
}

// status (and progress) for the window and the CLI
static void status(const double progress, const char *detail, const char *fmt, ...) {
    char text[sizeof(L.view.status)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&L.lock);
    snprintf(L.view.status, sizeof(L.view.status), "%s", text);
    snprintf(L.view.detail, sizeof(L.view.detail), "%s", detail ? detail : "");
    L.view.progress = progress;
    L.view.error = 0;
    pthread_mutex_unlock(&L.lock);
    printf("[launcher] %s%s%s\n", text, detail && *detail ? ": " : "", detail ? detail : "");
    fflush(stdout);
}

static void fail(const int can_play, const char *detail, const char *fmt, ...) {
    char text[sizeof(L.view.status)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&L.lock);
    snprintf(L.view.status, sizeof(L.view.status), "%s", text);
    snprintf(L.view.detail, sizeof(L.view.detail), "%s", detail ? detail : "");
    L.view.error = 1;
    L.view.can_play = can_play;
    L.can_play = can_play;
    pthread_mutex_unlock(&L.lock);
    fprintf(stderr, "[launcher] error: %s%s%s\n", text, detail && *detail ? ": " : "", detail ? detail : "");
}

static void fmt_mb(char *out, const size_t n, const int64_t done, const int64_t total) {
    const double mb = 1024.0 * 1024.0;
    if (total > 0) snprintf(out, n, "%.1f / %.1f MB", (double) done / mb, (double) total / mb);
    else snprintf(out, n, "%.1f MB", (double) done / mb);
}

static void fmt_eta(char *out, const size_t n, const double s) {
    if (s < 60) snprintf(out, n, "%d s left", (int) s + 1);
    else if (s < 3600) snprintf(out, n, "%d min %02d s left", (int) s / 60, (int) s % 60);
    else snprintf(out, n, "%d h %02d min left", (int) s / 3600, (int) s / 60 % 60);
}

// Speed: bytes over the last ~1 s window, smoothed (alpha 0.3) so the number does not jump with
// every finished batch. ETA = remaining / smoothed speed, shown only after 3 s of transfer and
// once the speed is known (an early ETA is a guess).
static void transfer_progress(const char *what, const int64_t done, const int64_t total, const size_t files_done,
                              const size_t files_total) {
    const double t = now_s();
    if (t - L.speed_t0 >= 1.0) {
        const double sample = (double) (done - L.speed_b0) / (t - L.speed_t0);
        L.speed = L.speed > 0 ? 0.7 * L.speed + 0.3 * (sample > 0 ? sample : 0) : (sample > 0 ? sample : 0);
        L.speed_t0 = t;
        L.speed_b0 = done;
    }
    char detail[256], mb[64], eta[48] = "";
    fmt_mb(mb, sizeof(mb), done, total);
    if (total > done && L.speed > 1024 && t - L.xfer_t0 >= 3.0) {
        char e[40];
        fmt_eta(e, sizeof(e), (double) (total - done) / L.speed);
        snprintf(eta, sizeof(eta), "  -  %s", e);
    }
    if (files_total)
        snprintf(detail, sizeof(detail), "%s  -  %zu / %zu files  -  %.1f MB/s%s", mb, files_done, files_total, L.speed / 1048576.0, eta);
    else snprintf(detail, sizeof(detail), "%s  -  %.1f MB/s%s", mb, L.speed / 1048576.0, eta);
    pthread_mutex_lock(&L.lock);
    snprintf(L.view.status, sizeof(L.view.status), "%s", what);
    snprintf(L.view.detail, sizeof(L.view.detail), "%s", detail);
    L.view.progress = total > 0 ? (double) done / (double) total : -1;
    pthread_mutex_unlock(&L.lock);
}

static void transfer_start(void) {
    L.speed_t0 = L.xfer_t0 = now_s();
    L.speed_b0 = 0;
    L.speed = 0;
}

// ------------------------------------------------------------------ build identity

static void self_build(KnUpdateBuild *b) {
    const KnBuildInfo *bi = kn_build_info();
    memset(b, 0, sizeof(*b));
    snprintf(b->describe, sizeof(b->describe), "%s", bi->describe);
    snprintf(b->commit, sizeof(b->commit), "%s", bi->commit);
    b->commit_time = bi->commit_time;
    snprintf(b->build_time, sizeof(b->build_time), "%s", bi->build_time);
    b->proto_major = bi->protocol_major;
    b->proto_minor = bi->protocol_minor;
}

static const char *updates_url(void) {
    if (L.updates_url[0]) return L.updates_url;
    const char *env = getenv("KRUNKNATIVE_UPDATES_URL");
    return env && *env ? env : KN_UPDATE_DEFAULT_URL;
}

// ------------------------------------------------------------------ executable update

typedef struct {
    FILE *f;
    KnSha256 sha;
    int64_t done, total;
} PkgSink;

static int pkg_write(void *user, const void *data, const size_t len) {
    PkgSink *s = user;
    if (s->done + (int64_t) len > PACKAGE_MAX || fwrite(data, 1, len, s->f) != len) return 0;
    kn_sha256_update(&s->sha, data, len);
    s->done += (int64_t) len;
    transfer_progress("Downloading the update", s->done, s->total, 0, 0);
    return 1;
}

// The client inside the staged package starts on this machine (catches a build that needs a
// newer system than this one before anything is replaced).
static int staged_client_runs(const char *staged, char *why, const size_t whyn) {
    // the client's path relative to the root, inside the package
    char rel[KN_PATH_MAX], path[KN_PATH_MAX];
    const size_t rl = strlen(L.in.root);
    if (strncmp(L.in.client, L.in.root, rl) != 0) return 0;
    snprintf(rel, sizeof(rel), "%s", L.in.client + rl + (L.in.client[rl] == '/'));
    kn_fs_join(path, sizeof(path), staged, rel);
    if (!kn_fs_exists(path)) {
        snprintf(why, whyn, "the package has no %s", rel);
        return 0;
    }
    char out[1024];
    char *argv[] = {path, "--version", NULL};
    const int code = kn_proc_run(path, argv, 30000, out, sizeof(out));
    // "KrunkNative client": every client's --version has it (Krunker Box Office ones: "Krunker
    // Box Office (KrunkNative client) <build>"), so older and newer packages both pass
    if (code != 0 || !strstr(out, "KrunkNative client")) {
        out[strcspn(out, "\r\n")] = 0;
        snprintf(why, whyn, "the new client does not start here (exit %d%s%.200s)", code, out[0] ? ": " : "", out);
        return 0;
    }
    return 1;
}

static int update_executables(const KnUpdateManifest *m) {
    const KnUpdatePlatform *p = kn_update_manifest_platform(m, L.in.platform);
    if (!p || !p->has_update) {
        status(-1, NULL, "No update package for %s yet", L.in.platform);
        return 1;
    }
    char dl_dir[KN_PATH_MAX], pkg[KN_PATH_MAX], staged[KN_PATH_MAX];
    kn_install_recover(&L.in);
    kn_install_cleanup(&L.in);
    kn_fs_join(dl_dir, sizeof(dl_dir), L.in.state_dir, "download");
    kn_fs_join(pkg, sizeof(pkg), dl_dir, kn_update_archive_kind(L.in.platform)[0] == 'z' ? "package.zip" : "package.tar.gz");
    kn_fs_join(staged, sizeof(staged), L.in.state_dir, "staged");
    if (!kn_fs_mkdirs(dl_dir)) {
        fail(1, L.in.state_dir, "Cannot write to the install directory");
        return 0;
    }
    status(0, NULL, "Downloading the update (%s)", m->build.describe);
    PkgSink s = {.f = kn_fs_fopen(pkg, "wb"), .total = p->update.size};
    if (!s.f) {
        fail(1, pkg, "Cannot write the update");
        return 0;
    }
    kn_sha256_init(&s.sha);
    transfer_start();
    KnHttp *http = kn_http_new(30000);
    char err[512] = "";
    KnHttpSink sink = {.write = pkg_write, .user = &s, .cancel = &L.cancel};
    const int code = http ? kn_http_get(http, p->update.url, &sink, err, sizeof(err)) : 0;
    kn_http_free(http);
    const int closed = fclose(s.f) == 0;
    uint8_t digest[32];
    char hex[65];
    kn_sha256_final(&s.sha, digest);
    kn_sha256_hex(digest, hex);
    if (code != 200 || !closed) {
        if (code > 0) snprintf(err, sizeof(err), "HTTP %d for %s", code, p->update.url);
        fail(1, err, "Update download failed");
        kn_install_cleanup(&L.in);
        return 0;
    }
    if (s.done != p->update.size || strcmp(hex, p->update.sha256) != 0) {
        fail(1, "the package does not match the manifest's sha256", "Update download corrupt");
        kn_install_cleanup(&L.in);
        return 0;
    }
    status(-1, NULL, "Installing the update");
    if (!kn_archive_extract(pkg, staged, err, sizeof(err))) {
        fail(1, err, "Cannot unpack the update");
        kn_install_cleanup(&L.in);
        return 0;
    }
    if (!staged_client_runs(staged, err, sizeof(err))) {
        fail(1, err, "The update cannot run on this system");
        kn_install_cleanup(&L.in);
        return 0;
    }
    if (!kn_install_swap(&L.in, staged, err, sizeof(err))) {
        fail(1, err, "Installing the update failed (rolled back)");
        kn_install_cleanup(&L.in);
        return 0;
    }
    kn_install_cleanup(&L.in);
    L.updated = 1;
    printf("[launcher] updated to %s (%.12s)\n", m->build.describe, m->build.commit);
    return 1;
}

// ------------------------------------------------------------------ assets

static void assets_progress(void *user, const char *phase, const int64_t done, const int64_t total,
                            const size_t files_done, const size_t files_total) {
    (void) user;
    if (!strcmp(phase, "list")) status(-1, NULL, "Checking game content");
    else if (!strcmp(phase, "check")) {
        char d[96];
        snprintf(d, sizeof(d), "%zu / %zu files", files_done, files_total);
        pthread_mutex_lock(&L.lock);
        snprintf(L.view.status, sizeof(L.view.status), "Checking game files");
        snprintf(L.view.detail, sizeof(L.view.detail), "%s", d);
        L.view.progress = files_total ? (double) files_done / (double) files_total : -1;
        pthread_mutex_unlock(&L.lock);
    } else if (!strcmp(phase, "download")) {
        if (!L.download_started) {
            L.download_started = 1;
            transfer_start();
        }
        transfer_progress("Downloading game content", done, total, files_done, files_total);
    }
}

// The client needs at least its font and shaders: a quick sanity check of an assets dir.
static int assets_present(const char *dir) {
    char p[KN_PATH_MAX];
    kn_fs_join(p, sizeof(p), dir, "css/fonts/font2.ttf");
    return kn_fs_exists(p);
}

static void origins_add(const char *prefix, const char *url) {
    const size_t used = strlen(L.origins);
    if (url && *url && used + strlen(prefix) + strlen(url) + 2 < sizeof(L.origins))
        snprintf(L.origins + used, sizeof(L.origins) - used, "%s%s%s", used ? " " : "", prefix, url);
}

// The client's on-demand fetch of a file the sync missed or a user deleted (client/include/
// asset_fetch.h) uses the sync's origins (assets_sync.c setup_origins): --asset-mirrors, the
// manifest's mirrors (each Pages project with its path filter, so a file comes from its own
// project) or the built-in CDN first, then the manifest's objects on kru as the fallback.
static void note_origins(const KnUpdateManifest *m) {
    L.origins[0] = 0;
    if (!m->assets.version[0]) return;
    if (L.mirrors[0]) {
        if (strcmp(L.mirrors, "off") != 0) {
            char buf[sizeof(L.mirrors)];
            snprintf(buf, sizeof(buf), "%s", L.mirrors);
            for (char *t = strtok(buf, " ,"); t; t = strtok(NULL, " ,")) origins_add("", t);
        }
    } else if (m->assets.mirrors_set) {
        for (int i = 0; i < m->assets.mirror_count; i++) {
            char spec[KN_UPDATE_URL_MAX + KN_PATH_FILTER_MAX + 16];
            if (kn_origin_spec_format(spec, sizeof(spec), m->assets.mirrors[i].url, m->assets.mirrors[i].by_path,
                                      &m->assets.mirrors[i].filter))
                origins_add("", spec);
        }
    } else if (!strncmp(m->assets.objects, KN_ASSETS_DEFAULT_ORIGIN, strlen(KN_ASSETS_DEFAULT_ORIGIN))) {
        origins_add("", KN_ASSETS_DEFAULT_MIRROR);
    }
    origins_add("objects:", m->assets.objects);
}

static int sync_assets(const KnUpdateManifest *m) {
    char local[64];
    kn_assets_local_version(L.assets_dir, local, sizeof(local));
    if (!m->assets.version[0] || !strcmp(local, m->assets.version)) return 1;
    status(-1, NULL, local[0] ? "Updating game content" : "Downloading game content");
    L.download_started = 0;
    KnAssetsSyncOpts opts = {.progress = assets_progress, .cancel = &L.cancel, .threads = L.threads,
                             .mirrors = L.mirrors[0] ? L.mirrors : NULL};
    KnAssetsSyncResult r;
    char err[512] = "";
    if (!kn_assets_sync(L.assets_dir, m, &opts, &r, err, sizeof(err))) {
        fail(assets_present(L.assets_dir), err, "Game content download failed");
        return 0;
    }
    const double secs = L.download_started ? now_s() - L.xfer_t0 : 0;
    printf("[launcher] game content %s: %zu downloaded (%.1f MB%s%.0f s, %.1f MB/s), %zu removed, %zu unchanged, %zu verified\n",
           m->assets.version, r.fetched, (double) r.bytes / 1048576.0, secs > 0 ? " in " : "", secs > 0 ? secs : 0,
           secs > 0 ? (double) r.bytes / 1048576.0 / secs : 0, r.removed, r.kept, r.adopted);
    for (int g = 0; g < r.origin_count && r.unique; g++)
        printf("[launcher]   %s: %zu files, %.1f MB%s\n", r.origins[g].url, r.origins[g].files,
               (double) r.origins[g].bytes / 1048576.0, r.origins[g].disabled ? " (stopped using it)" : "");
    return 1;
}

// ------------------------------------------------------------------ the launcher phase

static void *work(void *arg) {
    (void) arg;
    L.ok = 0;
    L.can_play = 0;
    L.have_install = kn_install_locate(&L.in, L.root_override);
    if (!L.have_install) {
        fail(0, L.in.client, "client_bin is missing next to the launcher");
        goto out;
    }
    if (kn_install_recover(&L.in)) status(-1, NULL, "Rolled back an interrupted update");
    if (L.in.root_writable) kn_install_cleanup(&L.in);
    if (!kn_install_assets_dir(&L.in, L.assets_override, L.assets_dir, sizeof(L.assets_dir))) L.assets_dir[0] = 0;
    char checkout[KN_PATH_MAX];
    if (L.assets_dir[0] && !L.allow_checkout_assets && kn_install_dir_in_checkout(L.assets_dir, checkout, sizeof(checkout))) {
        // a dev run pointed at a repository's assets/: the sync would rewrite and delete files there
        fail(0, L.assets_dir, "The game content directory is inside the git checkout %s (use --allow-checkout-assets)", checkout);
        goto out;
    }

    KnUpdateManifest m;
    memset(&m, 0, sizeof(m));
    const char *url = updates_url();
    L.manifest_ok = 0;
    if (strcmp(url, "off") != 0) {
        status(-1, NULL, "Checking for updates");
        KnHttp *http = kn_http_new(10000);
        char *json = NULL, err[512] = "";
        size_t len = 0;
        const int code = http ? kn_http_get_mem(http, url, MANIFEST_MAX, &json, &len, err, sizeof(err)) : 0;
        kn_http_free(http);
        if (code == 200 && kn_update_manifest_parse(json, len, url, &m, err, sizeof(err))) L.manifest_ok = 1;
        else fprintf(stderr, "[launcher] update check failed: %s\n", code && code != 200 ? "HTTP error" : err);
        free(json);
    }
    KnUpdateBuild mine;
    self_build(&mine);
    if (L.manifest_ok) {
        L.state = kn_update_compare(&mine, &m.build, m.grace_s);
        snprintf(L.latest, sizeof(L.latest), "%s", m.build.describe);
        printf("[launcher] this build %s (%s), latest %s (%.12s): %s\n", mine.describe, mine.commit, m.build.describe,
               m.build.commit, kn_update_state_name(L.state));
    }
    if (L.check_only) {
        L.ok = 1;
        goto out_m;
    }
    if (L.manifest_ok && L.state != KN_UPDATE_CURRENT && !L.no_update) {
        if (!L.in.has_marker) {
            status(-1, NULL, "Development build: not updating");
        } else if (!kn_install_self_updatable(&L.in.marker)) {
            status(-1, kn_update_download_url(&m, L.in.platform, L.in.marker.package),
                   "Update %s available: update " KN_PRODUCT_NAME " with your package manager", m.build.describe);
        } else if (!L.in.root_writable) {
            status(-1, L.in.root, "Update available, but the install directory is read-only");
        } else if (!update_executables(&m)) {
            goto out_m;
        }
    }
    if (L.assets_dir[0]) {
        if (L.manifest_ok) note_origins(&m);
        if (L.manifest_ok && !sync_assets(&m)) goto out_m;
        // the package's code-coupled files (shaders, maps, ...) over the content: they match the binary
        char err[512] = "";
        const int copied = kn_install_package_assets(&L.in, L.assets_dir, err, sizeof(err));
        if (copied < 0) {
            fail(assets_present(L.assets_dir), err, "Cannot install the game files");
            goto out_m;
        }
        if (copied > 0) printf("[launcher] %d game file(s) from the package into %s\n", copied, L.assets_dir);
        if (!L.manifest_ok && !assets_present(L.assets_dir)) {
            fail(0, L.assets_dir, "Cannot reach the update server to download the game content");
            goto out_m;
        }
    }
    status(1, NULL, "Starting " KN_PRODUCT_NAME);
    L.ok = 1;
out_m:
    kn_update_manifest_free(&m);
out:
    pthread_mutex_lock(&L.lock);
    L.done = 1;
    pthread_mutex_unlock(&L.lock);
    return NULL;
}

static int run_phase(LauncherUi *ui) {
    for (;;) {
        L.done = 0;
        pthread_t t;
        if (pthread_create(&t, NULL, work, NULL) != 0) return 0;
        LauncherAction action = UI_NONE;
        for (;;) {
            pthread_mutex_lock(&L.lock);
            const int done = L.done;
            LauncherView v = L.view;
            pthread_mutex_unlock(&L.lock);
            if (ui) {
                action = launcher_ui_frame(ui, &v);
                if (action == UI_QUIT && !L.cancel) {
                    __atomic_store_n(&L.cancel, 1, __ATOMIC_RELAXED); // downloads stop; an install in progress finishes (or rolls back) first
                    launcher_ui_hide(ui);
                }
            } else {
                // CLI: progress lines at most once a second
                char line[sizeof(L.last_cli)];
                snprintf(line, sizeof(line), "%s %s", v.status, v.detail);
                static double last;
                if (v.progress >= 0 && strcmp(line, L.last_cli) != 0 && now_s() - last >= 1.0) {
                    printf("[launcher] %s: %s (%d%%)\n", v.status, v.detail, (int) (v.progress * 100));
                    fflush(stdout);
                    snprintf(L.last_cli, sizeof(L.last_cli), "%s", line);
                    last = now_s();
                }
#ifdef _WIN32
                Sleep(30);
#else
                const struct timespec ts = {0, 30000000};
                nanosleep(&ts, NULL);
#endif
            }
            if (done) break;
        }
        pthread_join(t, NULL);
        if (L.cancel) return 0;
        if (L.ok || !ui) return L.ok ? 1 : (L.can_play ? 2 : 0);
        // the client restarted itself for an update (--wait-pid: Update now, the auto-restart): a
        // failed update (e.g. the staged client does not run here) starts the installed client
        // again instead of waiting for a click; the client then keeps offering the download
        if (L.wait_pid > 0 && L.can_play) {
            printf("[launcher] the update failed: starting the installed client\n");
            return 2;
        }
        // the error stays on screen until the user picks
        for (;;) {
            pthread_mutex_lock(&L.lock);
            LauncherView v = L.view;
            pthread_mutex_unlock(&L.lock);
            action = launcher_ui_frame(ui, &v);
            if (action == UI_QUIT) return 0;
            if (action == UI_PLAY && L.can_play) return 2;
            if (action == UI_RETRY) break;
        }
        status(-1, NULL, "Retrying");
    }
}

// --wait-pid: the client that asked for the update is shutting down; give it up to 30 s.
static void wait_for_pid(const long pid) {
#ifdef _WIN32
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD) pid);
    if (h) {
        WaitForSingleObject(h, 30000);
        CloseHandle(h);
    }
#else
    for (int i = 0; i < 300 && kill((pid_t) pid, 0) == 0; i++) {
        const struct timespec ts = {0, 100000000};
        nanosleep(&ts, NULL);
    }
#endif
}

static void usage(void) {
    printf(KN_PRODUCT_NAME " launcher (updates the game, then starts it)\n"
           "Usage: krunknative [--cli|--headless] [--no-update] [--check] [--no-launch] [--wait-pid PID]\n"
           "                   [--updates-url URL|off] [--assets-dir DIR] [--threads N] [--asset-mirrors URLS|off]\n"
           "                   [--] [client args...]\n"
           "  --cli, --headless  no window: progress on stdout\n"
           "  --no-update        do not install a newer build (still syncs the game content)\n"
           "  --check            report whether a newer build exists (exit 3) and stop\n"
           "  --no-launch        update, but do not start the game\n"
           "  --updates-url URL  channel manifest (default %s; env KRUNKNATIVE_UPDATES_URL)\n"
           "  --assets-dir DIR   game content directory (env KRUNKNATIVE_ASSETS_DIR); refused inside a git\n"
           "                     checkout (the sync deletes files it does not list) unless\n"
           "  --allow-checkout-assets\n"
           "  --threads N        game content: connections per origin (default %d; env KRUNKNATIVE_ASSET_THREADS)\n"
           "  --asset-mirrors U  game content mirrors (by path) besides the objects, \"off\" = none\n"
           "                     (default: the manifest's; env KRUNKNATIVE_ASSET_MIRRORS)\n"
           "Other arguments are passed to client_bin.\n",
           KN_UPDATE_DEFAULT_URL, KN_ASSETS_THREADS_DEFAULT);
}

int main(int argc, char **argv) {
    pthread_mutex_init(&L.lock, NULL);
    L.view.progress = -1;
    L.client_args = calloc((size_t) argc + 2, sizeof(char *));
    if (!L.client_args) return 1;
    int rest = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (rest) L.client_args[L.client_argc++] = argv[i];
        else if (!strcmp(a, "--")) rest = 1;
        else if (!strcmp(a, "--cli") || !strcmp(a, "--headless")) L.cli = 1;
        else if (!strcmp(a, "--no-update")) L.no_update = 1;
        else if (!strcmp(a, "--allow-checkout-assets")) L.allow_checkout_assets = 1;
        else if (!strcmp(a, "--check")) L.check_only = L.cli = 1;
        else if (!strcmp(a, "--no-launch")) L.no_launch = 1;
        else if (!strcmp(a, "--updates-url") && i + 1 < argc) snprintf(L.updates_url, sizeof(L.updates_url), "%s", argv[++i]);
        else if (!strcmp(a, "--assets-dir") && i + 1 < argc) snprintf(L.assets_override, sizeof(L.assets_override), "%s", argv[++i]);
        else if (!strcmp(a, "--threads") && i + 1 < argc) L.threads = atoi(argv[++i]);
        else if (!strcmp(a, "--asset-mirrors") && i + 1 < argc) snprintf(L.mirrors, sizeof(L.mirrors), "%s", argv[++i]);
        else if (!strcmp(a, "--wait-pid") && i + 1 < argc) L.wait_pid = strtol(argv[++i], NULL, 10);
        else if (!strcmp(a, "--root") && i + 1 < argc) snprintf(L.root_override, sizeof(L.root_override), "%s", argv[++i]); // tests
        else if (!strcmp(a, "--version")) {
            printf(KN_PRODUCT_NAME " launcher %s\n", kn_build_string());
            return 0;
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage();
            return 0;
        } else L.client_args[L.client_argc++] = argv[i];
    }
#ifdef _WIN32
    // a GUI-subsystem exe: --cli prints to the console it was started from, if any
    if (L.cli && AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
#endif
    if (!L.assets_override[0]) {
        const char *env = getenv("KRUNKNATIVE_ASSETS_DIR");
        if (env && *env) snprintf(L.assets_override, sizeof(L.assets_override), "%s", env);
    }
    if (!L.threads && getenv("KRUNKNATIVE_ASSET_THREADS")) L.threads = atoi(getenv("KRUNKNATIVE_ASSET_THREADS"));
    if (!L.mirrors[0] && getenv("KRUNKNATIVE_ASSET_MIRRORS"))
        snprintf(L.mirrors, sizeof(L.mirrors), "%s", getenv("KRUNKNATIVE_ASSET_MIRRORS"));
    printf(KN_PRODUCT_NAME " launcher %s\n", kn_build_string());
    if (L.wait_pid > 0) wait_for_pid(L.wait_pid);
    char ua[128];
    snprintf(ua, sizeof(ua), "KrunkNative-launcher/%s (%s)", kn_build_info()->describe, kn_update_platform_default());
    kn_http_set_user_agent(ua);

    char ui_dir[KN_PATH_MAX] = "";
    {
        KnInstall probe;
        kn_install_locate(&probe, L.root_override);
        kn_fs_join(ui_dir, sizeof(ui_dir), probe.marker_dir, "ui");
    }
    LauncherUi *ui = L.cli ? NULL : launcher_ui_open(ui_dir);
    if (!ui && !L.cli) printf("[launcher] no window available: running in --cli mode\n");
    const int phase = run_phase(ui);
    if (L.check_only) {
        if (ui) launcher_ui_close(ui);
        if (!L.manifest_ok) return 2;
        printf("[launcher] %s\n", L.state == KN_UPDATE_CURRENT ? "up to date" : "update available");
        return L.state == KN_UPDATE_CURRENT ? 0 : 3;
    }
    if (ui) launcher_ui_close(ui);
    if (!phase) return 2;
    if (L.no_launch) return 0;

    // client_bin: cwd = the directory with ui/, the game content dir and this launcher's path
    char **cargv = calloc((size_t) L.client_argc + 2, sizeof(char *));
    if (!cargv) return 2;
    cargv[0] = L.in.client;
    for (int i = 0; i < L.client_argc; i++) cargv[i + 1] = L.client_args[i];
    static char env_assets[KN_PATH_MAX + 32], env_launcher[KN_PATH_MAX + 32], env_origins[sizeof(L.origins) + 32];
    const char *env[4] = {NULL, NULL, NULL, NULL};
    int ne = 0;
    if (L.assets_dir[0]) {
        snprintf(env_assets, sizeof(env_assets), "KRUNKNATIVE_ASSETS_DIR=%s", L.assets_dir);
        env[ne++] = env_assets;
        const char *own = getenv("KRUNKNATIVE_ASSET_ORIGINS"); // the user's choice wins
        if (L.origins[0] && !(own && *own)) {
            snprintf(env_origins, sizeof(env_origins), "KRUNKNATIVE_ASSET_ORIGINS=%s", L.origins);
            env[ne++] = env_origins;
        }
    }
    snprintf(env_launcher, sizeof(env_launcher), "KRUNKNATIVE_LAUNCHER=%s", L.in.exe);
    env[ne++] = env_launcher;
    printf("[launcher] starting %s\n", L.in.client);
    fflush(stdout);
    char err[512] = "";
    if (!kn_proc_start_client(L.in.client, cargv, L.in.marker_dir, env, err, sizeof(err))) {
        fprintf(stderr, "[launcher] cannot start the game: %s\n", err);
        return 2;
    }
    return 0; // Windows: the client runs on its own
}
