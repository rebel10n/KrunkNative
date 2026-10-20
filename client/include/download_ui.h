#pragma once
// The Download window (client/src/ui/download_ui.c): v7.1.4's showWindow(25) (windows[24]
// "client_download", the header's "Download Our Client" image) listing KrunkNative's packages
// on the GitHub release `client-latest` instead of Krunker's installers (krunker.zip):
// Windows / macOS / Linux launcher packages (tools/release/package-client.sh), the Android app
// (BoxOffice-android.apk) and, for iPhone / iPad, Add to Home Screen (the web build's PWA
// manifest) plus an unsigned .ipa for sideloading. The visitor's platform is detected (web:
// navigator.userAgent / userAgentData; native: kn-install.json, else the build's platform) and
// its package is shown first, highlighted, with every other platform listed under it.

// showWindow(25)'s markup: ui/windows/25.html with its #knDownloads placeholder filled.
char *download_window_gen(void);
// ?download (index.html?download, the hub's app badges): open the window once the menu is up.
void download_ui_query(const char *query);
// Per frame from client_menu_tick() (DOM ready): the pending ?download, the web platform probe.
void download_ui_tick(void);
