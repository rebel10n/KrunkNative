#pragma once
// The K.P.D replay player (client/src/net/demo_play.c, doc/v10/replays.md 2.4): Krunker v10.0.7's demo
// playback (deob-10.0.7.js:201566-201963: load, injectInit, start, tick, seek, _applySeek) over a Box
// Office demo file (shared/include/demo.h). It is a transport like the offline sandbox (net/offline.c):
// the recorded packets go into client_net_queue_packet at the playhead, so the client's own handlers
// build the game exactly as when the recorder spectated live; client_net_send sends nothing meanwhile
// (client->replay). Main thread only.
#include <client.h>
#include <demo.h>
#include <stddef.h>
#include <stdint.h>

// watchDemo's load + start (:229703-229744): parse (a copy of data), hideIdentities, the suspect (the
// call of call_token, else the first suspect), its segments; the live game is left (its transport
// released, the offline game stopped), then the first keyframe is applied. 0 with err ("Invalid Demo
// File" for a file demo_parse refuses) and nothing changed.
int demo_play_start(Client*, const uint8_t *data, size_t length, const char *demo_token, const char *call_token,
                    char *err, size_t err_size);
// A demo file on disk (client_bin --demo <file>[:<call token>], KRUNKNATIVE_DEMO): demo_play_start.
int demo_play_start_file(Client*, const char *path, const char *call_token, char *err, size_t err_size);
// Exit Replay: the replay's game is left; the transport released at the start comes back (the server
// it was on, or the offline game), else the menu stays over an empty world. Krunker reloads the page.
void demo_play_stop(Client*);

// Main loop, before client_tick_net (next to client_offline_pump): tick (:201874-201905).
void demo_play_pump(Client*, float delta);

int demo_play_active(void);
int demo_play_waiting(void);              // waitingWorld: an INIT of the demo is being built
int demo_play_playing(void);              // playing (not paused, not ended)
void demo_play_set_playing(int playing);  // setPlaying
float demo_play_speed(void);              // 0.5, 1, 2 or 4
void demo_play_cycle_speed(void);         // the speed button: the next of 0.5x 1x 2x 4x (:229783-229787)
uint32_t demo_play_playhead_ms(void);
uint32_t demo_play_duration_ms(void);
void demo_play_seek(Client*, uint32_t t_ms); // seek (:201918-201941), clamped to the suspect segments
int demo_play_suspect_uid(void);          // suspectSid, -1 none (or no replay)
const DemoSegment *demo_play_segments(size_t *count);
const char *demo_play_token(void);        // the demo's tok ("" for a local file)
const char *demo_play_call_token(void);   // the call token it was opened with ("" none: no voting)
const char *demo_play_meta(void);         // the meta JSON ("" none)
// onRoundStart: 1 once after the world of an INIT was reached or a seek re-added the players (the menu
// goes down and the camera follows the suspect, :229724-229731)
int demo_play_take_round_start(void);
// 1 once a frame since the last call when playback ended (onEnd) or the playhead moved by a seek
int demo_play_take_ended(void);

// The seek whitelist's complement (doc/v10/replays.md 2.4): packets of a moment, not of the state
// (shots, projectiles, explosions, sounds, chat, kill feed, medals, popups), left out while a seek
// fast-forwards from the keyframe; kills but the last three too.
int demo_play_transient_packet(uint8_t type);
// Control packets never applied (:201566-201578): pings, errors, redirects, the auth handshake.
int demo_play_control_packet(uint8_t type);
