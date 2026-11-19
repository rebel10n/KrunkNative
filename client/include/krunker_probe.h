#pragma once

// The input-bank probe (doc/v10/input-bank-probe.md): a measurement of Krunker's official server, for the two
// values Box Office cannot source (shared/src/player.c PLAYER_INPUT_BANK_MAX / PLAYER_INPUT_BANK_RATE: the bundle
// only declares inputBank / inputBankAt, deob-10.0.7.js:171267-171268). Off unless KRUNKNATIVE_INPUT_BANK_PROBE is
// set, and only on the official-server link (krunker_net.c / krunker_link.c): never against Box Office's servers.
//
// While it runs it drives the local player (move forward / back / stand, krunker_link_input_fields) through a fixed
// schedule of walks. A "control" walk sends its frames as they are made; a "held" walk keeps every outbound frame
// in the socket's queue for the walk's length (what a lag switch does) and then lets them go in one burst. Every
// server position ("l" SYNC_PLAYER), every Input Failed ("f") and every hold / release is written to a TSV that
// tools/krunker/input_bank_probe.py turns into the bank's cap (the longest held walk the server still moves in
// full) and refill (how much of a second held walk moves after a short rest).
//
// Environment:
//   KRUNKNATIVE_INPUT_BANK_PROBE       comma list of walk lengths in ms (e.g. 100,250,500,750,1000,1500,2000):
//                                      each is walked once as a control and once held
//   KRUNKNATIVE_INPUT_BANK_PROBE_GAPS  optional comma list of rests in ms for the refill pairs (two held walks of
//                                      the longest length with that rest between them), e.g. 250,500,1000,2000
//   KRUNKNATIVE_INPUT_BANK_PROBE_LOG   the TSV path (default input_bank_probe.tsv in the working directory)
// The schedule starts 3 s after the first server position and ends standing; the log's last line is "done".

#include <stdint.h>

int krunker_probe_active(void);
// The server's position for input `ack` (on_sync_player). Arms the schedule on the first one.
void krunker_probe_on_sync(int32_t ack, const float position[3]);
// 1 while every outbound frame must stay queued (the socket's sender, out_flush).
int krunker_probe_hold(void);
// The sender let `frames` queued frames go after a hold.
void krunker_probe_released(int frames);
// The movement the probe wants this frame: -2 none (the player's own), -1 stand, else a move_dir (0 forward, 4 back).
int krunker_probe_move_dir(void);
// "f" SYNC_FAILED with its code.
void krunker_probe_input_failed(int code);
