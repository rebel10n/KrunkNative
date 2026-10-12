#pragma once
// Game -> audio glue (client/src/sound_events.c): Krunker v3.1.1's SOUND.play call sites,
// reproduced by diffing the client's player state once per frame instead of hooking the
// shared simulation (which stays free of client concerns).
#include <client.h>

// Per frame, after the game tick: listener = camera, map ambience/emitters, local-player
// events (shots, melee, reload, aim, swap, jump, land, slide, steps, bounce, damage) and
// remote players' footsteps. Ends with audio_update().
void sound_events_tick(Client *client, float now, float delta);

// Events the client cannot derive from state yet; call them from the packet handlers once
// the protocol carries them (Krunker index.js handlers named in sound_events.c).
void sound_events_hitmarker(void);                       // gF: hit_0
void sound_events_kill(int headshot);                    // gk: kill sound (headshot_0)
void sound_events_remote_shot(Client *client, const Player *shooter); // "s": play3D weapon sound
void sound_events_explosion(vec3 position);              // gb: play3D explosion
void sound_events_change_ambient(int ambient);           // WP-C "camb": changeAmbient(soundscape) (the next map resets it)

// Inline page handlers (client/src/ui/ui_sound.c) for ui_handlers.c's table:
// "playTick" -> tick_0 at 0.2, "SOUND.play" -> SOUND.play(name, volume, loop, rate).
#include <html_dom.h>
int ui_sound_play_tick(void *user, const HtmlEvent *ev, const HtmlArg *args, int argc);
int ui_sound_play(void *user, const HtmlEvent *ev, const HtmlArg *args, int argc);
