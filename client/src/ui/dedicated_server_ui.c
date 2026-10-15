// The Dedicated Server window (client/include/dedicated_server_ui.h): the Host Game flow's "Get the
// dedicated server". Everything is in the client: no web page to open.
//
// Laid out like the Download window (download_ui.c): a highlighted box with this computer's package
// (.knDlRec), the other platforms one row each (.knDlRow), then the how-to sections in the same dark
// palette (#menuWindow.dark, ui/css/krunknative.css), headed like its "Other platforms" (.knDlOtherHed).
// The packages are the assets of the rolling release `server-latest` (release.yml, replaced on every
// push to main), so the links never name a version; kn_brain's finder answers with the same URLs
// (`downloads`, services/brain/releases.c) and servers_ui.c's "Download server" opens them too.
#include <dedicated_server_ui.h>
#include <build_info.h>
#include <html_dom.h>
#include <i18n.h>
#include <ui_logic.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DS_REPO "https://github.com/rebel10n/KrunkNative"
#define DS_TAG "server-latest"
#define DS_BASE DS_REPO "/releases/download/" DS_TAG "/"
#define DS_PAGE DS_REPO "/releases/tag/" DS_TAG
#define DS_PORT "21015" // NET_DEFAULT_PORT (shared/include/shared.h): TCP game + UDP game / server-info query
#define DS_FINDER "https://matchmaker.krunk.io"

typedef enum { DS_OS_LINUX, DS_OS_WINDOWS, DS_OS_MAC } DsOs;

typedef struct {
    const char *id;   // krunknative-server-<id>.<ext> (package-server.sh platforms)
    DsOs os;
    const char *ext;  // tar.gz | zip
    const char *name; // the row / box title
    const char *desc; // requirements (package-server.sh README "Runtime")
} DsPkg;

// release.yml's release-notes rows, in its order
static const DsPkg PKGS[] = {
    {"linux-x86_64", DS_OS_LINUX, "tar.gz", "Linux x86_64", "Debian 12+, Ubuntu 22.04+ (OpenSSL 3): the one for a VPS"},
    {"linux-x86_64-bullseye", DS_OS_LINUX, "tar.gz", "Linux x86_64 (Debian 11)", "Debian 11 and other glibc 2.31 systems (OpenSSL 1.1)"},
    {"windows-x86_64", DS_OS_WINDOWS, "zip", "Windows 10 / 11 (x64)", "Windows 10 and 11, 64-bit: the DLLs it needs are in the zip"},
    {"macos-arm64", DS_OS_MAC, "tar.gz", "macOS (Apple silicon)", "macOS on M1 and later"},
};
#define PKG_COUNT ((int) (sizeof(PKGS) / sizeof(PKGS[0])))

// A package's requirements in the page's language (dynamic keys, listed for i18n_lint.py):
//   I18N("kn.dserver.pkg.linux-x86_64", "Debian 12+, Ubuntu 22.04+ (OpenSSL 3): the one for a VPS")
//   I18N("kn.dserver.pkg.linux-x86_64-bullseye", "Debian 11 and other glibc 2.31 systems (OpenSSL 1.1)")
//   I18N("kn.dserver.pkg.windows-x86_64", "Windows 10 and 11, 64-bit: the DLLs it needs are in the zip")
//   I18N("kn.dserver.pkg.macos-arm64", "macOS on M1 and later")
static const char *pkg_desc(const DsPkg *p) {
    char key[64];
    snprintf(key, sizeof(key), "kn.dserver.pkg.%s", p->id);
    return i18n_tr(key, p->desc);
}

static struct {
    int from_host_flow; // opened from the Host Game flow: the Back button returns there
} g_ds;

// ---------------------------------------------------------------- this computer

// The package for the machine the client runs on. The web build cannot tell what a server will run
// on (and most run on a rented Linux box): Linux x86_64. NULL: no prebuilt package (Linux on ARM,
// Intel Macs): the box then shows how to build it.
static const DsPkg *this_computer(int *native) {
    *native = 0;
#ifdef __EMSCRIPTEN__
    return &PKGS[0];
#else
    *native = 1;
#if defined(_WIN32)
    return &PKGS[2];
#elif defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    return &PKGS[3];
#elif defined(__linux__) && (defined(__x86_64__) || defined(__amd64__))
    return &PKGS[0];
#else
    return NULL;
#endif
#endif
}

// ---------------------------------------------------------------- markup helpers

static void ds_link(StrBuf *sb, const char *cls, const char *url, const char *label) {
    sb_appendf(sb, "<a class=\"%s\" href=\"", cls);
    sb_append_esc(sb, url);
    sb_append(sb, "\" target=\"_blank\">");
    sb_append_esc(sb, label);
    sb_append(sb, "</a>");
}

static void pkg_url(const DsPkg *p, const char *suffix, char *out, const size_t n) {
    snprintf(out, n, DS_BASE "krunknative-server-%s.%s%s", p->id, p->ext, suffix);
}

static const char *os_icon(const DsOs os) {
    switch (os) {
        case DS_OS_WINDOWS: return "<img class=\"knDlIcon\" src=\"./img/windows.png\">";
        case DS_OS_MAC: return "<img class=\"knDlIcon\" src=\"./img/mac.png\">";
        default: return "<img class=\"knDlIcon\" src=\"./img/linux.png\">";
    }
}

// A translated text, escaped, with {0}{1} around bold words and {2}{3} around a value to type
// (monospace).
static void text(StrBuf *sb, const char *translated) {
    StrBuf esc = {0};
    sb_append_esc(&esc, translated);
    char *e = sb_finish(&esc);
    size_t n = strlen(e) + 1; // each {N} becomes at most the longest argument
    for (const char *c = e; *c; c++) n += *c == '{' ? 64 : 0;
    char *out = malloc(n);
    if (out) {
        i18n_replace(out, n, e, 4, I18N_ARGV("<b>", "</b>", "<span style='font-family:monospace;font-size:1.15em;color:#fff'>", "</span>"));
        sb_append(sb, out);
        free(out);
    }
    free(e);
}

static void heading(StrBuf *sb, const char *id, const char *label) {
    sb_appendf(sb, "<div class=\"knDlOtherHed\" id=\"%s\" style=\"margin-top:18px;font-size:20px;color:#fff\">", id);
    sb_append_esc(sb, label);
    sb_append(sb, "</div>");
}

static void para(StrBuf *sb, const char *translated) {
    sb_append(sb, "<div class=\"knDlHow\" style=\"margin:0 4px 8px 4px\">");
    text(sb, translated);
    sb_append(sb, "</div>");
}

// One numbered step.
static void step(StrBuf *sb, const int n, const char *translated) {
    sb_appendf(sb, "<div class=\"knDlHow\" style=\"display:flex;margin:0 4px 6px 4px\"><div style=\"flex-shrink:0;width:26px;color:#fbc02d\">%d.</div><div>", n);
    text(sb, translated);
    sb_append(sb, "</div></div>");
}

// Lines to copy into a terminal (selectable, like the Host Game flow's command box).
static void code(StrBuf *sb, const char *lines) {
    sb_append(sb, "<div class=\"knDsCode\" style=\"font-family:monospace;font-size:14px;white-space:pre-wrap;word-break:break-all;"
                  "user-select:text;background:rgba(0,0,0,0.45);color:#eee;padding:10px 12px;border-radius:6px;margin:4px 4px 10px 4px\">");
    // one <div> a line: white-space is not relied on (the native engine folds the newlines)
    for (const char *l = lines; *l;) {
        const char *end = strchr(l, '\n');
        const size_t len = end ? (size_t) (end - l) : strlen(l);
        char line[512];
        snprintf(line, sizeof(line), "%.*s", (int) len, l);
        sb_append(sb, "<div style=\"font-family:monospace;user-select:text\">"); // main.css: GameFont, user-select none on every element
        sb_append_esc(sb, line);
        sb_append(sb, "</div>");
        l += len + (end ? 1 : 0);
    }
    sb_append(sb, "</div>");
}

// A two-column row: a key (monospace) or name, then its meaning.
static void row(StrBuf *sb, const int odd, const char *key, const int mono, const char *translated) {
    sb_appendf(sb, "<div class=\"knDlRow%s\" style=\"align-items:flex-start\"><div style=\"flex-shrink:0;width:190px;%s\">",
               odd ? " knDlOdd" : "", mono ? "font-family:monospace;color:#fff" : "");
    sb_append_esc(sb, key);
    sb_append(sb, "</div><div class=\"knDlRowName\" style=\"color:rgba(255,255,255,0.75)\">");
    text(sb, translated);
    sb_append(sb, "</div></div>");
}

// ---------------------------------------------------------------- sections

static void downloads(StrBuf *sb) {
    int native = 0;
    const DsPkg *rec = this_computer(&native);
    char url[256], label[96];

    sb_append(sb, "<div class=\"knDlRec\" id=\"knDsRec\">");
    sb_appendf(sb, "<div class=\"knDlRecTag\">%s</div>", native ? I18N("kn.dserver.thisComputer", "FOR THIS COMPUTER")
                                                                  : I18N("kn.dserver.recommended", "RECOMMENDED: A LINUX SERVER OR VPS"));
    if (rec) {
        sb_appendf(sb, "<div class=\"knDlRecHead\">%s<div><div class=\"knDlRecName\">", os_icon(rec->os));
        sb_append_esc(sb, rec->name);
        sb_appendf(sb, "</div><div class=\"knDlRecSub\">%s</div></div></div><div class=\"knDlBtns\">", pkg_desc(rec));
        pkg_url(rec, "", url, sizeof(url));
        char ext[16];
        snprintf(ext, sizeof(ext), ".%s", rec->ext);
        i18n_trf(label, sizeof(label), "kn.download.button", "Download {0}", 1, I18N_ARGV(ext));
        ds_link(sb, "knDlBtn", url, label);
        pkg_url(rec, ".sha256", url, sizeof(url));
        ds_link(sb, "knDlBtn knDlAlt", url, ".sha256");
        sb_append(sb, "</div><div class=\"knDlHow\">");
        if (rec->os == DS_OS_WINDOWS) {
            text(sb, I18N("kn.dserver.howWindows", "Unzip it anywhere, edit {2}cfg\\server.cfg{3} and run {0}server_bin.exe{1}. "
                                                   "Allow it when Windows Firewall asks (private and public networks)."));
        } else if (rec->os == DS_OS_MAC) {
            text(sb, I18N("kn.dserver.howMac", "Unpack it, edit {2}cfg/server.cfg{3} and run {2}./server_bin{3} in Terminal. "
                                               "It is not notarized: the first time, run {2}xattr -d com.apple.quarantine server_bin *.dylib{3} in its folder."));
        } else {
            text(sb, I18N("kn.dserver.howLinux", "Unpack it, edit {2}cfg/server.cfg{3} and run {2}./server_bin{3}. It needs libcurl and OpenSSL: "
                                                 "{2}sudo apt install libcurl4 libssl3 libcrypt1{3}."));
        }
        sb_append(sb, "</div>");
    } else {
        sb_appendf(sb, "<div class=\"knDlRecHead\">%s<div><div class=\"knDlRecName\">%s</div><div class=\"knDlRecSub\">%s</div></div></div>",
                   os_icon(DS_OS_LINUX), I18N("kn.dserver.noPackage", "No ready-made server for this computer"),
                   I18N("kn.dserver.noPackageSub", "Build it from source (below, Linux on ARM), or run it on a Linux x86_64 server"));
    }
    sb_append(sb, "</div>");

    sb_appendf(sb, "<div class=\"knDlOther\" id=\"knDsOther\"><div class=\"knDlOtherHed\">%s</div>",
               rec ? I18N("kn.download.otherPlatforms", "Other platforms") : I18N("kn.download.allPlatforms", "All platforms"));
    int odd = 0;
    for (int i = 0; i < PKG_COUNT; i++) {
        const DsPkg *p = &PKGS[i];
        if (p == rec) continue;
        sb_appendf(sb, "<div class=\"knDlRow%s\">%s<div class=\"knDlRowName\">", odd++ & 1 ? " knDlOdd" : "", os_icon(p->os));
        sb_append_esc(sb, p->name);
        sb_append(sb, "</div><div class=\"knDlRowLinks\">");
        pkg_url(p, "", url, sizeof(url));
        snprintf(label, sizeof(label), ".%s", p->ext);
        ds_link(sb, "knDlLink", url, label);
        pkg_url(p, ".sha256", url, sizeof(url));
        ds_link(sb, "knDlLink", url, ".sha256");
        sb_append(sb, "</div></div>");
    }
    sb_appendf(sb, "<div class=\"knDlRow%s\">%s<div class=\"knDlRowName\">%s</div>"
                   "<div class=\"knDlRowLinks\"><span class=\"knDlHint\">%s</span></div></div>",
               odd & 1 ? " knDlOdd" : "", os_icon(DS_OS_LINUX), I18N("kn.dserver.arm", "Linux aarch64 (ARM)"),
               I18N("kn.dserver.armHint", "build from source, below"));
    sb_append(sb, "</div><div class=\"knDlFoot\">");
    text(sb, I18N("kn.dserver.latest", "Rolling build {2}server-latest{3}: it matches the current game. Servers on an older build show as Outdated in the server browser."));
    sb_append(sb, "<br>");
    ds_link(sb, "knDlLink", DS_PAGE, I18N("kn.download.releaseNotes", "Release notes and SHA-256 checksums"));
    sb_append(sb, "</div>");
}

static void quick_start(StrBuf *sb) {
    heading(sb, "knDsQuick", I18N("kn.dserver.quick", "Quick start"));
    step(sb, 1, I18N("kn.dserver.quick1", "Download your platform's package above and unpack it anywhere."));
    step(sb, 2, I18N("kn.dserver.quick2", "Open {2}cfg/server.cfg{3} and set at least {2}hostname{3} and, on a public server, "
                                          "{2}public_ip{3} (the address players connect to). Every key is explained in {2}server.cfg.example{3}."));
    step(sb, 3, I18N("kn.dserver.quick3", "Run {2}./server_bin{3} in that folder ({2}server_bin.exe{3} on Windows). Or paste the command from "
                                          "Host Game's last step: it carries the map, mode and rules you picked."));
    step(sb, 4, I18N("kn.dserver.quick4", "Open port {0}" DS_PORT "{1} for TCP and UDP (below), then join from another network: "
                                          "Servers, Direct Connect, {2}your-ip:" DS_PORT "{3}."));
    code(sb, "tar -xzf krunknative-server-linux-x86_64.tar.gz\n"
             "cd krunknative-server-linux-x86_64\n"
             "nano cfg/server.cfg      # hostname, public_ip, mapcycle, mode\n"
             "./server_bin             # ./server_bin --help lists every flag");
    para(sb, I18N("kn.dserver.console", "The window it runs in is its console: {2}status{3}, {2}players{3}, {2}kick{3}, {2}map{3}, {2}say{3}, "
                                        "{2}help{3}. Ctrl+C lets the round finish, then stops it. To upgrade, copy only the new "
                                        "{2}server_bin{3} over: {2}cfg/server.cfg{3} is yours."));
}

static void config_keys(StrBuf *sb) {
    heading(sb, "knDsCfg", I18N("kn.dserver.cfg", "server.cfg"));
    para(sb, I18N("kn.dserver.cfgIntro", "One {2}key value{3} per line, {2}//{3} comments, quotes around values with spaces. "
                                         "Flags on the command line win over the file."));
    int odd = 0;
    row(sb, odd++ & 1, "hostname", 1, I18N("kn.dserver.cfg.hostname", "The server's name in the server browser."));
    row(sb, odd++ & 1, "port", 1, I18N("kn.dserver.cfg.port", "Game port, TCP and UDP (default " DS_PORT ")."));
    row(sb, odd++ & 1, "public_ip", 1, I18N("kn.dserver.cfg.publicIp", "The address players connect to: your public IP or a domain. Leave it unset only for LAN games."));
    row(sb, odd++ & 1, "maxplayers", 1, I18N("kn.dserver.cfg.maxplayers", "Lobby size (1 to 31, default 8)."));
    row(sb, odd++ & 1, "sv_password", 1, I18N("kn.dserver.cfg.password", "Players need it to join; {2}\"\"{3} = open."));
    row(sb, odd++ & 1, "mapcycle", 1, I18N("kn.dserver.cfg.mapcycle", "Maps by name, comma separated: {2}Burg,Sandstorm,Kanji{3}."));
    row(sb, odd++ & 1, "mode", 1, I18N("kn.dserver.cfg.mode", "{2}ffa{3}, {2}tdm{3}, {2}ctf{3}, {2}point{3}, ... or {2}rotation{3} (the public rotation)."));
    row(sb, odd++ & 1, "bots / bot_quota", 1, I18N("kn.dserver.cfg.bots", "Keep N bots in the lobby, or fill it up to N players with bots that leave as people join."));
    row(sb, odd++ & 1, "region", 1, I18N("kn.dserver.cfg.region", "Region code shown in the list."));
    row(sb, odd++ & 1, "finder_url", 1, I18N("kn.dserver.cfg.finder", "{2}" DS_FINDER "{3} lists it in the server browser (below)."));
    row(sb, odd++ & 1, "online_mode", 1, I18N("kn.dserver.cfg.online", "{2}1{3}: signed-in players are checked with the account service, so names cannot be faked. Guests still join."));
    row(sb, odd++ & 1, "rcon_password", 1, I18N("kn.dserver.cfg.rcon", "With {2}rcon_port{3}: a remote console on 127.0.0.1 ({2}server_bin --rcon{3}). Never forward it."));
    row(sb, odd++ & 1, "game.<setting>", 1, I18N("kn.dserver.cfg.game", "Any Host Game setting, e.g. {2}game.gravMlt 0.5{3}."));
}

static void port_forwarding(StrBuf *sb) {
    heading(sb, "knDsPorts", I18N("kn.dserver.ports", "Port forwarding"));
    para(sb, I18N("kn.dserver.portsIntro", "Players outside your home network reach the server only through port {0}" DS_PORT "{1}, "
                                           "{0}TCP and UDP{1}: the game uses both, and the server browser checks the server over UDP."));
    step(sb, 1, I18N("kn.dserver.ports1", "Give the computer a fixed local address: a DHCP reservation in the router (e.g. {2}192.168.1.50{3})."));
    step(sb, 2, I18N("kn.dserver.ports2", "Open the router's page (usually {2}192.168.0.1{3} or {2}192.168.1.1{3}), find Port Forwarding "
                                          "(also called NAT, Virtual Server or Applications) and add {2}" DS_PORT "{3} for TCP and UDP to that address, "
                                          "same port."));
    step(sb, 3, I18N("kn.dserver.ports3", "Let it through the computer's firewall: allow server_bin when Windows asks, or "
                                          "{2}sudo ufw allow " DS_PORT "{3} on Linux. On a VPS, open it in the provider's firewall as well."));
    step(sb, 4, I18N("kn.dserver.ports4", "Set {2}public_ip{3} to your public address (a \"what is my IP\" site shows it) and test from "
                                          "another network: {2}python3 kn-query.py your-ip " DS_PORT "{3} prints the server's name and map."));
    para(sb, I18N("kn.dserver.cgnat", "If the router's WAN address is not your public IP, your provider shares one address between "
                                      "customers (CGNAT) and forwarding cannot work: ask them for a public IPv4, or use a VPS."));
}

static void coordinator(StrBuf *sb) {
    heading(sb, "knDsList", I18N("kn.dserver.list", "Get listed (coordinator)"));
    step(sb, 1, I18N("kn.dserver.list1", "Add {2}finder_url " DS_FINDER "{3} to {2}cfg/server.cfg{3} and restart. The server sends a "
                                         "heartbeat every minute."));
    step(sb, 2, I18N("kn.dserver.list2", "The coordinator then asks the server for its info over UDP " DS_PORT ". Once it answers, the "
                                         "server shows under Community Servers in everyone's server browser. No answer (port closed, "
                                         "wrong {2}public_ip{3}): not listed."));
    step(sb, 3, I18N("kn.dserver.list3", "Optional: {2}online_mode 1{3} checks signed-in players with the account service "
                                         "(coordinator.krunk.io), so their names and accounts are real. Guests can still play."));
    para(sb, I18N("kn.dserver.network", "Running servers for a community network with its own coordinator? Its operator gives you "
                                        "{2}coordinator_url{3} and {2}coordinator_secret{3}: that links the server to their accounts, skins "
                                        "and matchmaking. Without them the server runs standalone."));
}

static void vps(StrBuf *sb) {
    heading(sb, "knDsVps", I18N("kn.dserver.vps", "Host on a VPS"));
    para(sb, I18N("kn.dserver.vpsIntro", "A small rented Linux server keeps a lobby up around the clock without touching your home "
                                         "network. One {2}server_bin{3} is one lobby and needs little: about 110 MB of memory and a few "
                                         "percent of one CPU with a full lobby, so the cheapest plan is enough. Pick a location close "
                                         "to your players."));
    int odd = 0;
    // name, plan, approximate price per month, link (the provider's pricing page)
    static const struct {
        const char *name, *plan, *price, *url;
    } VPS[] = {
        {"DigitalOcean", "Basic Droplet, 1 vCPU, 1 GB", "~ $6", "https://www.digitalocean.com/pricing/droplets"},
        {"Hetzner Cloud", "Shared vCPU (CX / CPX), 2+ GB, EU", "~ \xe2\x82\xac" "6", "https://www.hetzner.com/cloud/"},
        {"Gcore", "Cloud Standard VM", "~ \xe2\x82\xac" "9", "https://gcore.com/cloud/virtual-machines"},
        {"Hetzner (ARM)", "Cloud CAX11 Ampere, 2 vCPU, 4 GB", "~ \xe2\x82\xac" "6.50", "https://www.hetzner.com/cloud/"},
        {"Oracle (ARM)", "Cloud Always Free, Ampere A1", "free", "https://www.oracle.com/cloud/free/"},
    };
    sb_appendf(sb, "<div class=\"knDlOther\" id=\"knDsVpsList\">"
                   "<div class=\"knDlRow\" style=\"color:rgba(255,255,255,0.5);font-size:15px\"><div style=\"flex-shrink:0;width:190px\">%s</div>"
                   "<div class=\"knDlRowName\">%s</div><div class=\"knDlRowLinks\">%s</div></div>",
               I18N("kn.dserver.vps.provider", "Provider"), I18N("kn.dserver.vps.plan", "Smallest plan that fits"), I18N("kn.dserver.vps.price", "Per month"));
    for (size_t i = 0; i < sizeof(VPS) / sizeof(VPS[0]); i++) {
        sb_appendf(sb, "<div class=\"knDlRow%s\"><div style=\"flex-shrink:0;width:190px\">", odd++ & 1 ? "" : " knDlOdd");
        ds_link(sb, "knDlLink knDsVpsLink", VPS[i].url, VPS[i].name);
        sb_append(sb, "</div><div class=\"knDlRowName\" style=\"color:rgba(255,255,255,0.75)\">");
        sb_append_esc(sb, VPS[i].plan);
        sb_append(sb, "</div><div class=\"knDlRowLinks\" style=\"color:#fbc02d\">");
        sb_append_esc(sb, strcmp(VPS[i].price, "free") ? VPS[i].price : I18N("kn.dserver.vps.free", "free"));
        sb_append(sb, "</div></div>");
    }
    sb_append(sb, "</div>");
    // the date the prices were checked: update it with the table
    sb_append(sb, "<div class=\"knDlNote\" id=\"knDsPrices\" style=\"margin:6px 4px 10px 4px\">");
    text(sb, I18N("kn.dserver.vps.note", "Approximate prices as of October 2026 (checked 2026-10-15), before VAT, for the smallest plan; "
                                         "providers change them often and some plans sell out. Check the provider's page before you buy. "
                                         "Krunker Box Office is not affiliated with any of them."));
    sb_append(sb, "</div>");
    para(sb, I18N("kn.dserver.vpsSetup", "On the VPS (Ubuntu or Debian), as a normal user:"));
    code(sb, "sudo apt install -y libcurl4 libssl3 libcrypt1 tmux\n"
             "curl -LO " DS_BASE "krunknative-server-linux-x86_64.tar.gz\n"
             "tar -xzf krunknative-server-linux-x86_64.tar.gz && cd krunknative-server-linux-x86_64\n"
             "nano cfg/server.cfg      # hostname, public_ip = the VPS address, finder_url\n"
             "sudo ufw allow " DS_PORT "\n"
             "tmux new -s krunker ./server_bin      # detach: Ctrl+B then D; back: tmux attach -t krunker");
    para(sb, I18N("kn.dserver.armBuild", "ARM servers (Ampere, Raspberry Pi) have no ready-made package yet: build it from source "
                                    "(a few minutes) and run it from the source folder, which has the maps."));
    code(sb, "sudo apt install -y git cmake ninja-build gcc g++ make pkg-config libssl-dev libcurl4-openssl-dev libcrypt-dev\n"
             "git clone --depth 1 " DS_REPO " && cd KrunkNative\n"
             "cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DKRUNKNATIVE_HTML_ENGINE=OFF -DGLFW_BUILD_X11=OFF -DGLFW_BUILD_WAYLAND=OFF\n"
             "cmake --build build --target server_bin\n"
             "cp config/server.cfg.example server.cfg && nano server.cfg\n"
             "./build/server_bin");
}

// ---------------------------------------------------------------- window

char *dedicated_server_window_gen(const int id, const char **header) {
    if (id != DEDICATED_SERVER_WINDOW) return NULL;
    if (header) *header = I18N("kn.dserver.header", "Server");
    StrBuf sb = {0};
    // Download window's head (ui/windows/25.html): centred setHed, then the one-line description
    sb_appendf(&sb, "<div class=\"setHed\" id=\"knDsHead\" style=\"text-align:center;margin-top:0px\">%s</div>",
               I18N("kn.dserver.title", "Dedicated Server"));
    sb_append(&sb, "<div class=\"setBodH\" style=\"padding-bottom:10px\">");
    text(&sb, I18N("kn.dserver.desc", "Run your own Krunker Box Office server on your computer or a VPS. Players find it in the "
                                       "server browser or join it by address."));
    sb_append(&sb, "</div>");
    if (g_ds.from_host_flow) {
        sb_appendf(&sb, "<div style=\"margin:0 5px 12px 5px\"><div class=\"button small\" id=\"knDsBack\" style=\"display:inline-block;cursor:pointer;"
                        "padding:8px 14px;border-radius:6px;background:rgba(255,255,255,0.12);color:#fff\" onmouseenter=\"playTick()\" "
                        "onclick=\"playSelect(0.1);knHostFlow('reopen')\">%s</div></div>",
                   I18N("kn.dserver.back", "Back to Host Game"));
    }
    downloads(&sb);
    quick_start(&sb);
    config_keys(&sb);
    port_forwarding(&sb);
    coordinator(&sb);
    vps(&sb);
    char proto[128];
    I18N_F(proto, "kn.dserver.protocol", "This client speaks protocol {0}.", kn_build_protocol());
    sb_append(&sb, "<div class=\"knDlFoot\">");
    sb_append_esc(&sb, proto);
    sb_append(&sb, " ");
    ds_link(&sb, "knDlLink", DS_REPO "/blob/main/doc/backend.md", I18N("kn.dserver.docs", "Full server documentation"));
    sb_append(&sb, "</div>");
    return sb_finish(&sb);
}

void dedicated_server_ui_open(const int from_host_flow) {
    g_ds.from_host_flow = from_host_flow;
    menu_show_window(DEDICATED_SERVER_WINDOW, 1);
}
