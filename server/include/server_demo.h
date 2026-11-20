#pragma once
// K.P.D demo recorder (server/src/demo_rec.c, doc/v10/replays.md 2.1). Every call runs on the server's
// main thread with g_server->lock held.
#include <server.h>
#include <stdint.h>

// Recording rules (Box Office choices: Krunker's lengths live on its servers)
#define DEMO_KEYFRAME_S 15.0  // a keyframe this often (and after every INIT)
#define DEMO_BEFORE_S 120.0   // history kept before a call
#define DEMO_AFTER_S 30.0     // recorded after the call before the demo is written
#define DEMO_CHUNK_BYTES (192u * 1024u) // the brain upload's slice (base64 in one internal-socket frame)

// Once per server tick (server_core_tick): starts the recorder once the game is ready and demos are on
// (config gameserver.demos and a brain or gameserver.demo_dir), cuts the tick's stream record, takes the
// keyframes, trims the window, writes the demos that are due.
void demo_rec_tick(void);
// A K.P.D call on `suspect` was accepted (votekick_handle_kpd): its demo is written DEMO_AFTER_S from now.
// call_type: v10's typ (0 public, 1 ranked, 2 comp, 3 custom).
void demo_rec_note_call(const ClientConnection *caller, const ClientConnection *suspect, int call_type);
// The server console's `demo <name> [seconds]`: a demo of that player (or bot) as a call would, cut after_s
// from now (0: at the next tick). 1 when the player was found and the recorder runs.
int demo_rec_cut(const char *name, double after_s);
// The brain recorded the call on `suspect_id` (votekick_kpd_recorded): its call id goes into the demo.
void demo_rec_call_recorded(int suspect_id, int64_t call_id);
// The suspect left: its pending demos are written now (nothing more of it to record).
void demo_rec_on_leave(const ClientConnection *client);
// Shutdown / sandbox stop: pending demos written, the recorder removed, memory freed.
void demo_rec_stop(void);
// The recorder connection's id (-1 none) and its stats, for tests and the CLI.
int demo_rec_recorder_id(void);
int demo_rec_written(void);
