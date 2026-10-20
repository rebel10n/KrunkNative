#pragma once
// The launcher's view of its install (launcher/src/install.c): where it lives, where the game
// content goes, the journaled executable swap and process helpers.
//
// Layout (packages, tools/release/package-client.sh):
//   <root>/krunknative (KrunkNative.exe), client_bin (client.exe), server_bin, libs, ui/,
//   kn-install.json; macOS: KrunkNative.app/Contents/MacOS/{KrunkNative,client_bin,...} and
//   Contents/Resources/{ui/,kn-install.json} with "root": "../..".
// State: <root>/.kn-update/ (staged package, backup of the replaced files, journal); removed
// again once an update is complete.
#include <kn_update.h>
#include <stddef.h>

#define KN_PATH_MAX 4096

typedef struct {
    char exe[KN_PATH_MAX];        // this launcher (symlinks resolved)
    char exe_dir[KN_PATH_MAX];
    int has_marker;
    KnInstallMarker marker;
    char marker_dir[KN_PATH_MAX]; // the directory with kn-install.json and ui/ (client cwd)
    char root[KN_PATH_MAX];       // the install root (marker "root" resolved)
    char platform[48];            // marker platform, else kn_update_platform_default()
    char client[KN_PATH_MAX];     // client_bin next to the launcher
    char state_dir[KN_PATH_MAX];  // <root>/.kn-update
    int root_writable;
    int self_updatable;           // marker, not deb/rpm, root writable
} KnInstall;

// Finds the install from the launcher's own path (root_override: tests). 1 = found a client.
int kn_install_locate(KnInstall *in, const char *root_override);
// The per-user data directory (created): ~/.local/share/krunknative, ~/Library/Application
// Support/KrunkNative, %LOCALAPPDATA%/KrunkNative. 1 ok.
int kn_install_user_data_dir(char *out, size_t n);
// Where the game content lives (created; absolute, trailing '/'). override = --assets-dir /
// KRUNKNATIVE_ASSETS_DIR. "" (returns 0) for a dev launcher without a marker and no override.
int kn_install_assets_dir(const KnInstall *in, const char *override, char *out, size_t n);
// The git checkout (a directory with .git, also a worktree's .git file) that holds `dir` or is
// below it, symlinks resolved: its root in `root`, 1 = inside one. The launcher refuses to sync
// game content there (a sync deletes files it does not list) unless --allow-checkout-assets.
int kn_install_dir_in_checkout(const char *dir, char *root, size_t n);

// Moves every file of the extracted package `staged` into the root (a ui/ directory next to a
// kn-install.json as a whole), keeping the replaced ones in <state>/backup until the journal
// commits; any failure rolls back. Only the package's own files are replaced: of assets/ just
// the code-coupled files it ships (shaders, maps, ...), never the game content. The
// running launcher is renamed aside like any other file (allowed on Windows too) and the new
// one runs from the next start.
int kn_install_swap(const KnInstall *in, const char *staged, char *err, size_t errn);
// Startup: rolls back a swap a crash interrupted (journal without its commit line), then
// removes leftovers (staging, backup). Returns 1 when it rolled something back.
int kn_install_recover(const KnInstall *in);
void kn_install_cleanup(const KnInstall *in);

// Copies the package's own assets files (<marker dir>/assets: shaders, maps, ...) into the
// game content dir when that is another directory (deb/rpm, macOS, read-only roots), where
// they differ (size, then sha256), so they always match the binary. Returns the number of
// files copied, -1 on an error (err).
int kn_install_package_assets(const KnInstall *in, const char *assets_dir, char *err, size_t errn);

// Runs exe with argv (argv[0] = exe), waits up to timeout_ms; stdout+stderr into out. The exit
// code, -1 when it could not run or timed out.
int kn_proc_run(const char *exe, char *const argv[], int timeout_ms, char *out, size_t outn);
// Starts the client (cwd, extra environment "KEY=VALUE" pairs, NULL-terminated): POSIX execv
// (does not return on success: same pid), Windows CreateProcess (returns 1; the caller exits). On
// Windows the client's output goes where this process's does (kn_console_init) and it opens no window; started
// from a terminal, it returns when the client has exited, as for a console program.
int kn_proc_start_client(const char *exe, char *const argv[], const char *cwd, const char *const env[], char *err, size_t errn);
// Starts a detached process (the client's "Update now" relaunching the launcher). 1 ok.
int kn_proc_spawn_detached(const char *exe, char *const argv[], const char *cwd);
// Where a GUI-subsystem program's output goes (Windows; no-op elsewhere): started from a terminal, the
// terminal (AttachConsole); otherwise <user data>/logs/<log_name>, replaced on each start. Output that is
// already set up (a redirect, the handles the launcher passed on) is kept. 1 = the console, 2 = the log
// file, 0 = unchanged.
int kn_console_init(const char *log_name);
