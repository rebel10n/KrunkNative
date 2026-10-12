#pragma once
// Packaged installs (tools/release/package-client.sh): client_bin resolves ui/ and assets/
// relative to the working directory (assets_path() is "./assets/", the menu "assets/../ui/").
// Started from somewhere else (/usr/bin/krunknative -> /opt/krunknative/client_bin, a desktop
// entry, Finder, a Windows shortcut without "Start in"), it changes to its install root first:
//   - the executable's own directory (symlinks resolved), or
//   - Contents/Resources when the executable is Contents/MacOS/<exe> of a macOS .app bundle,
// provided that directory has a ui/ directory. Nothing changes when the working directory already
// has ui/ or assets/, or is a cmake-build-* tree (assets_path()'s development layout).
//
// Call once at startup, before anything opens a file under assets_path(). Returns 1 when it
// changed directory (logged), 0 otherwise. No-op on the web.
int install_root_chdir(void);
