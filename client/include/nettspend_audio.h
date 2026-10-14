#pragma once
// Nettspend class audio (client/src/nettspend_audio.c; doc/v7/decisions.md "Nettspend"): the
// Purple Drank's "pour up" reload (a scream, the "drank, drank, drank" line and the pour), Money
// Spread's cash throw and the laced state's ambience, on top of client/src/audio.c. The clips are
// KrunkNative's own synthesized assets/sound/kn_*.mp3 (tools/audio/nettspend_clips.sh), outside
// Krunker's sound table. sound_events.c calls the event hooks from its per-frame state diffs
// (the local player) and the shot packet handler (other players); the laced hook is for the
// NET_EXT laced packet's handler.
#include <shared.h>  // vec3, WEAPON_ID_DRANK / WEAPON_ID_MONEY_SPREAD (weapons.h), CLASS_ID_NETTSPEND
#include <net_ext.h> // NET_EXT_KN_LACED, NetLacedPacket

// The clips (assets/sound/<name>.mp3)
#define NETTSPEND_SOUND_RELOAD "kn_drank_reload"     // scream + "drank, drank, drank" (1.46 s)
#define NETTSPEND_SOUND_POUR "kn_drank_pour"         // the pour (1.2 s)
#define NETTSPEND_SOUND_SPLASH "kn_drank_splash"     // one splash of the stream (0.22 s), every 220 ms while spraying
#define NETTSPEND_SOUND_MONEY "kn_money_throw"       // the cash throw (0.5 s)
#define NETTSPEND_SOUND_LACED "kn_laced_ambience"    // the laced drone, loops (6 s)

// Volumes (Howl vol before the category factor; the weapon category like Krunker's reload_1 at
// 0.25 and weapon shots at 0.85). Others' sounds are play3D with Krunker's otherSoundMlt.
#define NETTSPEND_RELOAD_VOLUME 0.55f
#define NETTSPEND_POUR_VOLUME 0.35f
#define NETTSPEND_MONEY_VOLUME 0.6f
#define NETTSPEND_LACED_VOLUME 0.5f
#define NETTSPEND_REMOTE_MONEY_VOLUME 0.7f

// The weapon ids this module answers for (the rest of the weapons keep Krunker's sounds).
int nettspend_is_drank(int weapon_id);
int nettspend_is_money_spread(int weapon_id);

// The local player started reloading `weapon_id` (reload_timer went from 0 up): the Drank plays
// the pour and the scream + line (2D, like Krunker's reload_1). Returns 1 when it was the Drank.
int nettspend_audio_reload_start(int weapon_id);
// The local player fired `weapon_id`: Money Spread's throw (rate 0.92-1.08). Returns 1 if played.
int nettspend_audio_shot(int weapon_id);
// Another player at `position` fired `weapon_id` (the shot packet): Money Spread's throw as a 3D
// one-shot. Returns 1 if played. (Another player's reload is not on the wire, so the pour up is
// heard by its own player only, like Krunker's reload sounds.)
int nettspend_audio_remote_shot(int weapon_id, vec3 position);

// The local player got laced for `duration_s` (the server's NET_EXT laced packet; a second one
// extends it): the ambience loops until then. 0 or less stops it now.
void nettspend_audio_laced(float duration_s);
// A NET_EXT_KN_LACED body (NetLacedPacket {uid, duration_ms}) from the game event handler
// (client/src/net/game_events.c): starts / extends the ambience when `uid` is our own player
// (`my_uid`); other players' spells make no sound here. Returns 1 when it was ours.
int nettspend_audio_on_laced(const void *body, unsigned body_length, int my_uid);
// The stream's per-shot sound: the Drank's Weapon.sound names the pour clip (shared/src/weapons/
// kn_drank.c, the pour-up's), so the 220 ms spray path (sound_events.c) plays the short splash
// instead of stacking 1.2 s pours. Returns the clip name for `weapon_id`, NULL for other weapons.
const char *nettspend_audio_stream_sound(int weapon_id);
int nettspend_audio_laced_active(void); // the ambience is looping
// Per frame (sound_events_tick): ends the laced loop when its time is up. `now` is any monotonic
// clock in seconds (sound_events' own).
void nettspend_audio_tick(float now);
// Round / session start: the laced loop stops, nothing else is kept.
void nettspend_audio_reset(void);
// Queue the clips for decoding (sound_events' preload).
void nettspend_audio_preload(void);
