#pragma once
// Game mode HUD and client mirror of the server's mode state (client/src/ui/hud_modes.c):
// the team scores bar / CTF flag status (#teamScores), #gameMessage (gmsg waitTimers,
// flag messages), #livesCount, the #teamName pill, #blockerDiv (lockPlayer), the action
// feed lines ("ac") and the end screen's team totals + mode endStats table. Krunker
// v3.1.1 index.js initGame / updateTeamScore / updateFlagInfo / updateGameMessage /
// updateTeam / updateLives / actionFeed / endGame markup (ui/index.html ids).
#include <client.h>

int  hud_modes_handle_packet(Client*, const NetPacket*); // 1: a mode packet (NET_PACKET_MODE_STATE..MEDAL)
int  hud_modes_handle_event(Client*, int type, const void *payload, uint16_t length); // same, typed payload (offline game)
void hud_modes_reset(Client*);                            // INIT: a new round / session
void hud_modes_tick(Client*);                             // per frame: keep the DOM in sync

// Mirrored state for the overlay / renderer (world positions, -1 / 0 when none).
int  hud_modes_flag_count(void);
int  hud_modes_flag(int index, vec3 *position, int *team, int *carrier_uid);
int  hud_modes_active_objective(const Client*, vec3 *center, vec3 *half); // 1 when a hardpoint is up
int  hud_modes_spawnable(int index, vec3 *position, int *team);           // spawnables ("crsp"), 0 = slot empty
int  hud_modes_spawnable_type(int index, int *data_c);                    // WP-D: its medals.js type, -1 = slot empty
#define HUD_MODES_MAX_SPAWNABLES MODE_MAX_SPAWNABLES
const char *hud_modes_game_message(void);                 // what #gameMessage shows ("" none)

// WP-D: the v7.1.4 mode state the HUD shows (ui-v7 owns the DOM): team scores 1..5 (sdm), rounds
// ("rds"), lockout ("lkt"), the local KR Tags ("krt"), the last "tx" / "Switching Sides" popup.
// Objective uid / time, dom zones and per-player kranked / headMlt live in client->game.mode.
typedef struct {
    int team_scores[MODE_TEAMS];
    int team_round_wins[MODE_TEAMS];
    int rounds_left;            // -1: no rounds packet yet
    int lockout_ms;
    int dog_tags;
    const char *popup_title;
    const char *popup_text;
    int popup_seq;              // bumps on every new popup
} HudModesV7;
const HudModesV7 *hud_modes_v7(void);
