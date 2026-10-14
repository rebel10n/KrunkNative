// The Download window (client/include/download_ui.h): showWindow(25).
//
// v7.1.4's window (windows[24], label "client_download", header "Client") links Krunker's own
// installers; KrunkNative keeps its header and description (ui/windows/25.html, made by
// tools/ui-import/windows_v7.py) and fills #knDownloads with its own packages, all assets of
// the rolling GitHub release `client-latest` (.github/workflows/client.yml):
//   krunknative-client-<platform>.<ext>   the launcher packages (tools/release/package-client.sh)
//   krunknative-android.apk               the Android app (platform/android, signed release)
//   krunknative-ios-unsigned.ipa          the iOS app, unsigned (AltStore / Sideloadly re-sign it)
// iPhone / iPad players are pointed at Add to Home Screen first: the web build's PWA manifest
// (platform/web) makes krunk.io a fullscreen home screen app with no signing involved.
//
// Layout: "Recommended for your device" (the detected platform's package, its download
// buttons and how to start it), then "Other platforms" (every other package, one row each)
// and the release page (checksums, notes). Detection: the web build reads the browser's
// navigator.userAgent / userAgentData (EM_JS glue below, no UI logic in ui/js); natively it
// is the install's kn-install.json (platform + package kind), else the build's platform.
#include <download_ui.h>
#include <ui_logic.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#include <kn_fs.h>
#include <kn_update.h>
#endif

#define DL_REPO "https://github.com/rebel10n/KrunkNative"
#define DL_TAG "client-latest"
#define DL_BASE DL_REPO "/releases/download/" DL_TAG "/"
#define DL_PAGE DL_REPO "/releases/tag/" DL_TAG
#define DL_PLACEHOLDER "<div id=\"knDownloads\"></div>"
#define DL_EXT_MAX 4
#define DL_REC_MAX 2

typedef enum { DL_OS_UNKNOWN, DL_OS_WINDOWS, DL_OS_MAC, DL_OS_LINUX, DL_OS_ANDROID, DL_OS_IOS } DlOs;

typedef struct {
    const char *id;               // platform id (kn_update_platform_default), "android", "ios"
    DlOs os;
    const char *arch;             // "amd64" / "aarch64", NULL = any
    const char *name;             // "Other platforms" row
    const char *desc;             // under the platform name when recommended
    const char *exts[DL_EXT_MAX]; // formats on the release, preferred first
} DlPkg;

// The platforms of client.yml's release notes, with the same requirements.
static const DlPkg PKGS[] = {
    {"windows-amd64", DL_OS_WINDOWS, "amd64", "Windows 10 / 11 (x64)", "Windows 10 and 11, 64-bit", {"zip"}},
    {"macos-aarch64", DL_OS_MAC, "aarch64", "macOS (Apple silicon)", "macOS 11 or newer, Apple silicon (M1 and later)", {"zip", "tar.gz"}},
    {"macos-amd64", DL_OS_MAC, "amd64", "macOS (Intel)", "macOS 11 or newer, Intel", {"zip", "tar.gz"}},
    {"linux-amd64-polyfilled", DL_OS_LINUX, "amd64", "Linux x86_64 (any distro)", "x86_64, any distribution (glibc 2.17+)", {"deb", "rpm", "tar.gz", "zip"}},
    {"linux-amd64", DL_OS_LINUX, "amd64", "Linux x86_64 (glibc 2.35+)", "x86_64 for current distros (glibc 2.35+, OpenSSL 3)", {"deb", "rpm", "tar.gz", "zip"}},
    {"linux-aarch64-polyfilled", DL_OS_LINUX, "aarch64", "Linux aarch64 (any distro)", "aarch64, any distribution (glibc 2.17+)", {"deb", "rpm", "tar.gz", "zip"}},
    {"linux-aarch64", DL_OS_LINUX, "aarch64", "Linux aarch64 (glibc 2.35+)", "aarch64 for current distros (glibc 2.35+, OpenSSL 3)", {"deb", "rpm", "tar.gz", "zip"}},
    {"android", DL_OS_ANDROID, NULL, "Android", "Android 7.0 or newer", {"apk"}},
    {"ios", DL_OS_IOS, NULL, "iPhone / iPad", "Add Krunker Box Office to your Home Screen", {"ipa"}},
};
#define PKG_COUNT ((int) (sizeof(PKGS) / sizeof(PKGS[0])))

typedef struct {
    DlOs os;
    char arch[16];     // "" = unknown
    char platform[48]; // native: the exact package platform
    char kind[16];     // native: the installed package kind (kn-install.json "package")
    int native, app, standalone;
} DlDevice;

static struct {
    int pending;   // ?download seen, window not opened yet
    int probed;    // web: userAgentData.getHighEntropyValues started
} g_dl;

// ---------------------------------------------------------------- files and links

static void pkg_file(const DlPkg *p, const char *ext, char *out, const size_t n) {
    if (p->os == DL_OS_ANDROID) snprintf(out, n, "krunknative-android.apk");
    else if (p->os == DL_OS_IOS) snprintf(out, n, "krunknative-ios-unsigned.ipa");
    else snprintf(out, n, "krunknative-client-%s.%s", p->id, ext);
}

static void pkg_url(const DlPkg *p, const char *ext, char *out, const size_t n) {
    char file[96];
    pkg_file(p, ext, file, sizeof(file));
    snprintf(out, n, DL_BASE "%s", file);
}

static const DlPkg *pkg_find(const char *id) {
    for (int i = 0; i < PKG_COUNT; i++) {
        if (!strcmp(PKGS[i].id, id)) return &PKGS[i];
    }
    return NULL;
}

static const char *os_icon(const DlOs os) {
    switch (os) {
        case DL_OS_WINDOWS: return "<img class=\"knDlIcon\" src=\"./img/windows.png\">";
        case DL_OS_MAC: return "<img class=\"knDlIcon\" src=\"./img/mac.png\">";
        case DL_OS_LINUX: return "<img class=\"knDlIcon\" src=\"./img/linux.png\">";
        case DL_OS_ANDROID: return "<span class=\"material-icons knDlIcon knDlMi\" style=\"color:#3ddc84\">android</span>";
        case DL_OS_IOS: return "<span class=\"material-icons knDlIcon knDlMi\" style=\"color:#fff\">phone_iphone</span>";
        default: return "";
    }
}

static const char *os_name(const DlOs os) {
    switch (os) {
        case DL_OS_WINDOWS: return "Windows";
        case DL_OS_MAC: return "macOS";
        case DL_OS_LINUX: return "Linux";
        case DL_OS_ANDROID: return "Android";
        case DL_OS_IOS: return "iPhone / iPad";
        default: return "";
    }
}

static DlOs os_of_platform(const char *id) {
    if (!strncmp(id, "windows-", 8)) return DL_OS_WINDOWS;
    if (!strncmp(id, "macos-", 6)) return DL_OS_MAC;
    if (!strncmp(id, "linux-", 6)) return DL_OS_LINUX;
    return DL_OS_UNKNOWN;
}

// ---------------------------------------------------------------- detection

#ifdef __EMSCRIPTEN__
// userAgentData's architecture is only async (getHighEntropyValues): started with the menu so
// the window, opened later, reads it synchronously. Chromium's reduced user agent says
// "Linux x86_64" / "Intel Mac OS X" on every machine, so this is the reliable arch there.
EM_JS(void, kn_dl_probe_start, (void), {
    try {
        var uad = navigator.userAgentData;
        if (Module.knDlArch !== undefined || !uad || !uad.getHighEntropyValues) return;
        Module.knDlArch = "";
        uad.getHighEntropyValues(["architecture", "bitness"]).then(function (v) {
            var a = String(v.architecture || ""), b = String(v.bitness || "64");
            Module.knDlArch = b === "64" ? (a === "arm" ? "aarch64" : a === "x86" ? "amd64" : "") : "";
        }, function () {});
    } catch (e) {}
});

// "<os> <arch|-> <in the Android app> <home screen app>". Macs: every browser freezes the Mac user
// agent at "Intel Mac OS X", so the WebGL renderer tells Apple silicon ("Apple M1", the ANGLE
// string of Chromium) from Intel / AMD GPUs; Safari reports "Apple GPU" everywhere: unknown.
EM_JS(char *, kn_dl_probe, (void), {
    var os = "unknown", arch = "", app = 0, standalone = 0;
    try {
        var ua = String(navigator.userAgent || "");
        var uad = navigator.userAgentData;
        var plat = String((uad && uad.platform) || navigator.platform || "");
        app = /KrunkNativeApp/.test(ua) ? 1 : 0;
        if (/Android/i.test(ua)) os = "android";
        else if (/iPhone|iPad|iPod/.test(ua) || (/Macintosh/.test(ua) && navigator.maxTouchPoints > 1)) os = "ios";
        else if (/Windows/i.test(ua) || /^Win/i.test(plat)) os = "windows";
        else if (/Mac OS X|Macintosh/.test(ua) || /^Mac/i.test(plat)) os = "macos";
        else if (/Linux|X11|CrOS/i.test(ua) || /Linux/i.test(plat)) os = "linux";
        var both = ua + " " + plat;
        if (Module.knDlArch) arch = Module.knDlArch;
        else if (/aarch64|arm64|armv8/i.test(both)) arch = "aarch64";
        else if (os === "macos") {
            var gl = (typeof GLctx !== "undefined" && GLctx) || document.createElement("canvas").getContext("webgl");
            var ext = gl && gl.getExtension("WEBGL_debug_renderer_info");
            var r = ext ? String(gl.getParameter(ext.UNMASKED_RENDERER_WEBGL)) : "";
            if (/Apple M[0-9]/.test(r)) arch = "aarch64";
            else if (/Intel|AMD|Radeon|NVIDIA|GeForce/i.test(r)) arch = "amd64";
        } else if (/x86_64|x64|Win64|WOW64|amd64|i686/i.test(both)) arch = "amd64";
        standalone = navigator.standalone === true || (window.matchMedia && window.matchMedia("(display-mode: standalone)").matches) ? 1 : 0;
    } catch (e) {}
    return stringToNewUTF8(os + " " + (arch || "-") + " " + app + " " + standalone);
});

#endif

static void detect(DlDevice *d) {
    memset(d, 0, sizeof(*d));
#ifdef __EMSCRIPTEN__
    char *probe = kn_dl_probe();
    char os[16] = "", arch[16] = "";
    int app = 0, standalone = 0;
    if (probe && sscanf(probe, "%15s %15s %d %d", os, arch, &app, &standalone) == 4) {
        d->os = !strcmp(os, "windows") ? DL_OS_WINDOWS : !strcmp(os, "macos") ? DL_OS_MAC : !strcmp(os, "linux") ? DL_OS_LINUX
              : !strcmp(os, "android") ? DL_OS_ANDROID : !strcmp(os, "ios") ? DL_OS_IOS : DL_OS_UNKNOWN;
        if (strcmp(arch, "-") != 0) snprintf(d->arch, sizeof(d->arch), "%s", arch);
        d->app = app;
        d->standalone = standalone;
    }
    free(probe);
#else
    // the running install: kn-install.json (next to ui/, the client's cwd; update_ui.c reads
    // the same file), else a dev build of this platform
    d->native = 1;
    KnInstallMarker m;
    memset(&m, 0, sizeof(m));
    char *json = NULL;
    size_t len = 0;
    const int has_marker = kn_fs_read(KN_INSTALL_MARKER, &json, &len, 65536) && kn_install_marker_parse(json, len, &m);
    free(json);
    snprintf(d->platform, sizeof(d->platform), "%s", has_marker && m.platform[0] ? m.platform : kn_update_platform_default());
    if (has_marker) snprintf(d->kind, sizeof(d->kind), "%s", m.package);
    d->os = os_of_platform(d->platform);
    snprintf(d->arch, sizeof(d->arch), "%s", strstr(d->platform, "aarch64") ? "aarch64" : "amd64");
#endif
}

// The recommended package(s): one, or both Macs when the browser hides the architecture.
static int recommend(const DlDevice *d, const DlPkg **out) {
    if (d->native && pkg_find(d->platform)) {
        out[0] = pkg_find(d->platform);
        return 1;
    }
    switch (d->os) {
        case DL_OS_WINDOWS: out[0] = pkg_find("windows-amd64"); return 1; // Windows on Arm runs x64
        case DL_OS_MAC:
            if (d->arch[0]) {
                out[0] = pkg_find(!strcmp(d->arch, "amd64") ? "macos-amd64" : "macos-aarch64");
                return 1;
            }
            out[0] = pkg_find("macos-aarch64");
            out[1] = pkg_find("macos-amd64");
            return 2;
        case DL_OS_LINUX: // a browser cannot tell the distro: the polyfilled build runs on all of them
            out[0] = pkg_find(!strcmp(d->arch, "aarch64") ? "linux-aarch64-polyfilled" : "linux-amd64-polyfilled");
            return 1;
        case DL_OS_ANDROID: out[0] = pkg_find("android"); return 1;
        case DL_OS_IOS: out[0] = pkg_find("ios"); return 1;
        default: return 0;
    }
}

// ---------------------------------------------------------------- markup

// A package's description in the page's language: kn.download.<id>.desc (dynamic keys, named
// here for tools/i18n-import/i18n_lint.py, which reads the I18N(key, english) pairs anywhere):
//   I18N("kn.download.windows-amd64.desc", "Windows 10 and 11, 64-bit")
//   I18N("kn.download.macos-aarch64.desc", "macOS 11 or newer, Apple silicon (M1 and later)")
//   I18N("kn.download.macos-amd64.desc", "macOS 11 or newer, Intel")
//   I18N("kn.download.linux-amd64-polyfilled.desc", "x86_64, any distribution (glibc 2.17+)")
//   I18N("kn.download.linux-amd64.desc", "x86_64 for current distros (glibc 2.35+, OpenSSL 3)")
//   I18N("kn.download.linux-aarch64-polyfilled.desc", "aarch64, any distribution (glibc 2.17+)")
//   I18N("kn.download.linux-aarch64.desc", "aarch64 for current distros (glibc 2.35+, OpenSSL 3)")
//   I18N("kn.download.android.desc", "Android 7.0 or newer")
//   I18N("kn.download.ios.desc", "Add Krunker Box Office to your Home Screen")
static const char *pkg_desc(const DlPkg *p) {
    char key[96];
    snprintf(key, sizeof(key), "kn.download.%s.desc", p->id);
    return i18n_tr(key, p->desc);
}

static void ext_label(const DlPkg *p, const char *ext, char *out, const size_t n) {
    if (!strcmp(ext, "deb")) snprintf(out, n, ".deb (Debian, Ubuntu)");
    else if (!strcmp(ext, "rpm")) snprintf(out, n, ".rpm (Fedora, openSUSE)");
    else if (!strcmp(ext, "tar.gz") && p->os == DL_OS_LINUX) snprintf(out, n, "%s", I18N("kn.download.tarAnyDistro", ".tar.gz (any distro)"));
    else snprintf(out, n, ".%s", ext);
}

static void dl_link(StrBuf *sb, const char *cls, const char *url, const char *label) {
    sb_appendf(sb, "<a class=\"%s\" href=\"", cls);
    sb_append_esc(sb, url);
    sb_append(sb, "\" target=\"_blank\">");
    sb_append(sb, label);
    sb_append(sb, "</a>");
}

static void rec_buttons(StrBuf *sb, const DlDevice *d, const DlPkg *p, const int several) {
    char url[256], label[96];
    if (several) sb_appendf(sb, "<div class=\"knDlRecPkg\">%s</div>", pkg_desc(p));
    sb_append(sb, "<div class=\"knDlBtns\">");
    // the installed package kind first (deb / rpm installs update through their package)
    int order[DL_EXT_MAX], n = 0;
    for (int i = 0; i < DL_EXT_MAX && p->exts[i]; i++) {
        if (d->kind[0] && !strcmp(p->exts[i], d->kind)) order[n++] = i;
    }
    for (int i = 0; i < DL_EXT_MAX && p->exts[i]; i++) {
        if (!(d->kind[0] && !strcmp(p->exts[i], d->kind))) order[n++] = i;
    }
    for (int k = 0; k < n; k++) {
        const char *ext = p->exts[order[k]];
        pkg_url(p, ext, url, sizeof(url));
        if (p->os == DL_OS_ANDROID)
            snprintf(label, sizeof(label), "%s", d->app ? I18N("kn.download.apkLatest", "Download the latest APK") : I18N("kn.download.apkInstall", "Install the APK"));
        else if (p->os == DL_OS_IOS) continue; // Add to Home Screen first; the .ipa is a footnote
        else if (k == 0) {
            char e[48];
            ext_label(p, ext, e, sizeof(e));
            i18n_trf(label, sizeof(label), "kn.download.button", "Download {0}", 1, I18N_ARGV(e));
        } else {
            ext_label(p, ext, label, sizeof(label));
        }
        dl_link(sb, k == 0 ? "knDlBtn" : "knDlBtn knDlAlt", url, label);
    }
    sb_append(sb, "</div>");
}

// A how-to text: {0}/{1} around the file to run (bold), the rest escaped.
static void how(StrBuf *sb, const char *text) {
    StrBuf esc = {0};
    sb_append_esc(&esc, text);
    char *e = sb_finish(&esc);
    char out[1024];
    i18n_replace(out, sizeof(out), e, 2, I18N_ARGV("<b>", "</b>"));
    free(e);
    sb_append(sb, out);
}

static void rec_howto(StrBuf *sb, const DlDevice *d, const DlOs os) {
    sb_append(sb, "<div class=\"knDlHow\">");
    switch (os) {
        case DL_OS_WINDOWS:
            how(sb, I18N("kn.download.howWindows", "Unzip it anywhere and run {0}KrunkNative.exe{1}. The launcher downloads the game content "
                                                   "(about 1.7 GB) on the first start and keeps the client up to date."));
            break;
        case DL_OS_MAC:
            how(sb, I18N("kn.download.howMac", "Open {0}KrunkNative.app{1}. It is not notarized yet: the first time, right-click it and "
                                               "choose Open. The launcher downloads the game content (about 1.7 GB) and keeps it up to date."));
            break;
        case DL_OS_LINUX:
            how(sb, I18N("kn.download.howLinux", "Install the .deb / .rpm and start Krunker Box Office from the applications menu, or unpack the "
                                                 ".tar.gz and run {0}./krunknative{1}. The launcher downloads the game content (about 1.7 GB) "
                                                 "on the first start and keeps the client up to date."));
            break;
        case DL_OS_ANDROID:
            how(sb, d->app ? I18N("kn.download.howAndroidApp", "You are playing in the Krunker Box Office app: it always runs the latest game from krunk.io.")
                           : I18N("kn.download.howAndroid", "Open the downloaded file and allow your browser to install apps when asked. The app "
                                                             "plays krunk.io fullscreen with touch controls and controllers, always up to date."));
            break;
        case DL_OS_IOS:
            if (d->standalone) {
                how(sb, I18N("kn.download.howIosInstalled", "Krunker Box Office is on your Home Screen already: it always runs the latest version."));
            } else {
                how(sb, I18N("kn.download.howIos", "In Safari, tap Share (the square with an arrow), then {0}Add to Home Screen{1}. "
                                                   "Krunker Box Office then opens fullscreen like an app, always up to date."));
            }
            {
                char url[256];
                pkg_url(pkg_find("ios"), "ipa", url, sizeof(url));
                // "Sideloading (AltStore, Sideloadly)? {0}, signed on install with your Apple ID." with the link
                StrBuf link = {0};
                dl_link(&link, "knDlLink", url, I18N("kn.download.ipa", "Unsigned .ipa"));
                char *l = sb_finish(&link);
                char note[1024];
                i18n_trf(note, sizeof(note), "kn.download.sideload", "Sideloading (AltStore, Sideloadly)? {0}, signed on install with your Apple ID.", 1, I18N_ARGV(l));
                free(l);
                sb_appendf(sb, "<div class=\"knDlNote\">%s</div>", note);
            }
            break;
        default:
            break;
    }
    sb_append(sb, "</div>");
}

static void other_row(StrBuf *sb, const DlPkg *p, const int odd) {
    char url[256], label[32];
    sb_appendf(sb, "<div class=\"knDlRow%s\">%s<div class=\"knDlRowName\">%s</div><div class=\"knDlRowLinks\">",
               odd ? " knDlOdd" : "", os_icon(p->os), p->name);
    if (p->os == DL_OS_IOS) {
        // nothing to download on a computer: krunk.io in Safari, Share > Add to Home Screen
        sb_appendf(sb, "<span class=\"knDlHint\">%s</span>", I18N("kn.download.iosHint", "Safari: Add to Home Screen"));
    }
    for (int i = 0; i < DL_EXT_MAX && p->exts[i]; i++) {
        pkg_url(p, p->exts[i], url, sizeof(url));
        snprintf(label, sizeof(label), ".%s", p->exts[i]);
        dl_link(sb, "knDlLink", url, label);
    }
    sb_append(sb, "</div></div>");
}

static char *gen_list(const DlDevice *d) {
    StrBuf sb = {0};
    const DlPkg *rec[DL_REC_MAX] = {0};
    const int nrec = recommend(d, rec);
    if (nrec > 0) {
        sb_append(&sb, "<div class=\"knDlRec\">");
        sb_appendf(&sb, "<div class=\"knDlRecTag\">%s</div>", d->native ? I18N("kn.download.thisClient", "THIS CLIENT")
                                                                        : I18N("kn.download.recommended", "RECOMMENDED FOR YOUR DEVICE"));
        sb_appendf(&sb, "<div class=\"knDlRecHead\">%s<div><div class=\"knDlRecName\">%s</div>", os_icon(rec[0]->os), os_name(rec[0]->os));
        if (nrec == 1) sb_appendf(&sb, "<div class=\"knDlRecSub\">%s</div>", pkg_desc(rec[0]));
        else sb_appendf(&sb, "<div class=\"knDlRecSub\">%s</div>", I18N("kn.download.pickMac", "Pick your Mac's processor (About This Mac)"));
        sb_append(&sb, "</div></div>");
        for (int i = 0; i < nrec; i++) rec_buttons(&sb, d, rec[i], nrec > 1);
        rec_howto(&sb, d, rec[0]->os);
        sb_append(&sb, "</div>");
    }
    sb_appendf(&sb, "<div class=\"knDlOther\"><div class=\"knDlOtherHed\">%s</div>",
               nrec > 0 ? I18N("kn.download.otherPlatforms", "Other platforms") : I18N("kn.download.allPlatforms", "All platforms"));
    int row = 0;
    for (int i = 0; i < PKG_COUNT; i++) {
        int skip = 0;
        for (int r = 0; r < nrec; r++) skip |= rec[r] == &PKGS[i];
        if (!skip) other_row(&sb, &PKGS[i], row++ & 1);
    }
    sb_append(&sb, "</div><div class=\"knDlFoot\">");
    dl_link(&sb, "knDlLink", DL_PAGE, I18N("kn.download.releaseNotes", "Release notes and SHA-256 checksums"));
    sb_append(&sb, "</div>");
    return sb_finish(&sb);
}

char *download_window_gen(void) {
    DlDevice d;
    detect(&d);
    char *list = gen_list(&d);
    const char *tpl = ui_windows_html("25");
    StrBuf out = {0};
    if (tpl && strstr(tpl, DL_PLACEHOLDER)) {
        sb_append_replace_all(&out, tpl, DL_PLACEHOLDER, list);
    } else { // a template without the placeholder (an old ui/): the list after it
        sb_append(&out, tpl ? tpl : "");
        sb_append(&out, list);
    }
    free(list);
    return sb_finish(&out);
}

// ---------------------------------------------------------------- ?download

void download_ui_query(const char *query) {
    if (!query) return;
    for (const char *p = strstr(query, "download"); p; p = strstr(p + 1, "download")) {
        const int start = p == query || p[-1] == '?' || p[-1] == '&';
        const char end = p[8];
        if (start && (end == 0 || end == '=' || end == '&')) g_dl.pending = 1;
    }
}

void download_ui_tick(void) {
#ifdef __EMSCRIPTEN__
    if (!g_dl.probed) {
        g_dl.probed = 1;
        kn_dl_probe_start();
    }
#endif
#ifdef UI_SCENARIO_SUPPORTED
    if (ui_scenario_active()) g_dl.pending = 0; // renders show the scenario's window only
#endif
    if (g_dl.pending) {
        g_dl.pending = 0;
        menu_show_window(25, 1);
    }
}
