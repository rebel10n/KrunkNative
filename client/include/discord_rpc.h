#pragma once
// Discord Rich Presence over Discord's local IPC socket, the transport discord-rpc /
// discord_rich_presence use (glorp: src/app.rs + handlers.rs "rpc-update"; Krunker's
// v3.1.1 client: main/utilities.js discordRPC). No Discord SDK: the protocol is a framed
// JSON exchange on a Unix socket ($XDG_RUNTIME_DIR, $TMPDIR, ... /discord-ipc-N, plus
// the Flatpak/Snap sub-paths) or the Windows named pipe \\.\pipe\discord-ipc-N:
//   frame = uint32 opcode (0 handshake, 1 frame, 2 close, 3 ping, 4 pong) + uint32 length
//           (little endian) + UTF-8 JSON
//   handshake {"v":1,"client_id":"<application id>"}, then SET_ACTIVITY frames.
// Everything is non-blocking and polled from the main thread; when Discord is not
// running the client retries every few seconds. Native only (a browser cannot reach the
// IPC socket): every call is a no-op on the web.
//
// Application id: Discord's own "Krunker" entry (the official client's detectable game,
// so the profile reads "Playing Krunker"); KRUNKNATIVE_DISCORD_APP_ID overrides it,
// KRUNKNATIVE_DISCORD=0 disables RPC for the process (test runs).

// Enable/disable (the Client tab's "Discord Rich Presence" switch). Disabling clears
// the activity and closes the socket.
void discord_rpc_set_enabled(int enabled);
int  discord_rpc_enabled(void);

// The activity to show; NULL/"" fields are omitted. Sent when it changes (and again
// after a reconnect). `start_unix` > 0 shows the elapsed timer.
void discord_rpc_set_activity(const char *details, const char *state, long long start_unix);

// Main thread, a few times a second: connect/handshake, flush the activity, answer pings.
void discord_rpc_poll(void);

// Close the socket (process exit).
void discord_rpc_shutdown(void);
