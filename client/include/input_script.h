#pragma once
// Scripted input for movement / first-person feel parity tests (client/src/input_script.c).
// Inactive (every hook is a no-op) unless KRUNKNATIVE_INPUT_SCRIPT is set.
#include <client.h>

float input_script_delta(float delta);            // KRUNKNATIVE_FIXED_DELTA override of the frame delta (seconds)
void input_script_tick(Client*);                  // frame start: run due script events (keys, mouse, pose, play)
void input_script_adjust_input(Client*, Input*);  // after the look/move sampling: scripted look deltas
void input_script_frame_end(Client*, float delta, float frame_delta);// after game_tick + camera: per-frame log row
void input_script_frame_presented(Client*);       // the finished frame (page included): `frames` captures
// Page windows (hub / editor / viewer, app_mode.c): the script starts when the page is
// ready (app_mode_page_ready; `play`, `pose`, `tp`, `look` and `respawn` do nothing there),
// keys / mouse / wheel go through the page's callbacks, `shot` writes the page frame.
void input_script_page_tick(Client*);              // before the page tick
void input_script_page_frame_end(Client*, float frame_delta); // after the page is composited
