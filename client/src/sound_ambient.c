// The soundscape loop: Krunker v10.0.7's window.changeAmbient (deob-10.0.7.js:227084-227090) on SOUND.play /
// SOUND.stop / SOUND.fadeOut (client/src/audio.c).
#include <sound_ambient.h>
#include <audio.h>
#include <stdio.h>
#include <string.h>

static struct {
    int set;    // game.lastAmbS != null (never reset: the game object lives as long as the page)
    int id;     // lastAmbS: the parseInt'd soundscape or asset id, SOUND_AMBIENT_NAN for NaN
    int custom; // lastAmbSC: the id is a user asset ("asset:<id>")
} g_ambient;

// v10's ambient volumes by soundscape (deob-10.0.7.js:18456-18462), `|| 0.12` for the rest. changeAmbient looks a
// custom soundscape's asset id up in the same table.
float sound_ambient_volume(const int id) {
    switch (id) {
        case 6: return 0.3f;
        case 7: return 0.4f;
        case 15: return 0.04f;
        case 16: return 0.1f;
        case 17: return 0.2f;
        default: return 0.12f;
    }
}

// The Howl name of a soundscape: `custom ? id : "ambient_" + id`, a custom one a user asset (getSound with the asset
// flag: user-assets/<id>/sound.mp3, assetVolume). 0 when nothing can play under it (NaN: "ambient_NaN" and
// "NaN/sound.mp3" do not exist; neither does an asset id below 1).
static int ambient_name(const int id, const int custom, char *name, const size_t size) {
    if (id == SOUND_AMBIENT_NAN || (custom && id <= 0)) return 0;
    snprintf(name, size, custom ? "asset:%d" : "ambient_%d", id);
    return 1;
}

// SOUND.play(name, amvVols[id] || 0.12, true, 1, custom, undefined, fade ? {fadeIn: fade} : undefined). play() never
// plays "ambient_3" or "ambient_15" (deob-10.0.7.js:200768); a custom soundscape of asset 3 or 15 is not that name.
void sound_ambient_play(const int id, const int custom, const float fade_ms) {
    char name[32];
    if (!ambient_name(id, custom, name, sizeof(name))) return;
    if (!custom && (!strcmp(name, "ambient_3") || !strcmp(name, "ambient_15"))) return;
    if (fade_ms != 0.0f) audio_play_fade_in(name, sound_ambient_volume(id), 1, 1.0f, fade_ms);
    else audio_play(name, sound_ambient_volume(id), 1, 1.0f);
}

// window.changeAmbient(id, custom, fade) (deob-10.0.7.js:227084-227090), which "camb" calls and v10's
// updateEnvironment calls with a zone's ambIndC / ambFadeTime (:208571-208575). Nothing happens when parseInt(id) is
// the current id (NaN never is; the custom flag is not compared, so custom 5 after ambient_5 keeps ambient_5). Else
// the current loop fades out from amvVols[last] || 0.12 to 0 over the fade (it keeps looping silent, audio_fade_out)
// or stops without one, and the new one plays, faded in over the fade when there is one. fade_ms = 1000 * (fade || 0).
void sound_events_change_ambient(const int id, const int custom, const float fade_ms) {
    if (g_ambient.set && id != SOUND_AMBIENT_NAN && id == g_ambient.id) return;
    char name[32];
    if (g_ambient.set && ambient_name(g_ambient.id, g_ambient.custom, name, sizeof(name))) {
        if (fade_ms != 0.0f) audio_fade_out(name, sound_ambient_volume(g_ambient.id), fade_ms);
        else audio_stop(name);
    }
    g_ambient.set = 1;
    g_ambient.id = id;
    g_ambient.custom = custom;
    sound_ambient_play(id, custom, fade_ms);
}
