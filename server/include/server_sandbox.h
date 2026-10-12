#pragma once
// The offline sandbox: the real server core (server.h: connections, packet dispatch, chat
// commands, accounts gate, bots, triggers, interact, lag compensation, modes, the tick step)
// running inside the client process, the way Krunker v7.1.4 runs its ioSandbox / sandboxGame
// (src/libs/io-sandbox.js: a GameObj in the page fed the socket's packets). No sockets and no
// threads: a connection is a pair of in-memory byte queues carrying the exact framed packets
// of the TCP protocol ([type, length hi, length lo, payload], NET_PACKET_HEADER_SIZE), sent
// with net_send_packet() on NET_LOOPBACK_FD_BASE descriptors and read back through a
// NetPacketBuffer on both ends, and sandbox_step() runs the server on the caller's thread
// (the client's main loop, also on the web).
//
// Sandbox server config (the offline game's rules, server_bin's defaults otherwise): no
// brain, central auth, rcon, bans file, idle kicks or password; perms off and the loopback
// player counts as the host's own machine (DEV: every chat / console command); skins are not
// enforced, so everything is owned. One sandbox per process.
#include <cJSON.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const cJSON **maps;     // the map list (copied; the cJSON maps stay the caller's)
    size_t map_count;
    int map_index;          // first map, -1 random
    int mode_id;            // Krunker mode id of every round (config.modes = [id]), -1 Free for All
    unsigned short tick_rate; // 0 = 30 (gameserver tick_rate)
} SandboxConfig;

int sandbox_start(const SandboxConfig*); // 1 = running (a running sandbox is stopped first)
void sandbox_stop(void);                 // every connection closed, the game freed
int sandbox_running(void);

// A new connection, as if a client connected from 127.0.0.1. Returns the client end's
// descriptor (send to it with net_send_packet), -1 when full or not running.
int sandbox_connect(void);
// The client went away: the server drops the connection on its next step.
void sandbox_disconnect(int client_fd);
// Moves up to `size` bytes the server sent to this client end into `out`: the byte count,
// 0 when nothing is pending, -1 once the server closed the connection and everything was read.
int sandbox_receive(int client_fd, uint8_t *out, size_t size);

// Advances the sandbox clock by `seconds` (the client's frame delta), hands every pending
// packet to the server (net_poll_clients, like server_bin's net thread) and runs the server
// tick when one is due (tick_rate per second, at most one per step: one tick at 30 Hz inside
// the frame, never a burst after a hitch).
void sandbox_step(double seconds);

// Host controls (Host Game, the editor's Test Map, console `map` offline): every round on
// mode_id (-1 keeps the current modes) and this map now, the players who are alive respawn
// on it (net_change_map). add_map appends a map to the server's list (copy kept by the
// caller) and returns its index there, -1 on failure; replace_map swaps one entry.
void sandbox_host(int map_index, int mode_id);
int sandbox_add_map(const cJSON *map);
void sandbox_replace_map(int map_index, const cJSON *map);
