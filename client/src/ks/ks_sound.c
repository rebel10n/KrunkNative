// GAME.SOUND (doc/v7/krunkscript.md 3.1.14, 3.2.3; v7 createKrunkScriptApiContext.js:626-666,
// 2371-2422): user-asset sounds (assets/user-assets/<id>/sound.mp3, the "custom" volume channel
// assetVolume) through audio.c's voice handles, and the sound object.
#include "ks_client_internal.h"
#include <audio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int handle;   // audio voice handle (0 = gone)
    int aid;      // asset id
    float vol, rate;
} KsSound;

#define KSC_SOUND_MAX 256
static KsSound *g_sounds[KSC_SOUND_MAX];
static int g_sound_count;

static KsSound *sound_of(void *native) { return native; }

static KsValue snd_get_volume(KsScript *s, void *native, uint32_t id) {
    (void) s; (void) id;
    return ks_num(sound_of(native)->vol);
}

static void snd_set_volume(KsScript *s, void *native, uint32_t id, KsValue v) {
    (void) s; (void) id;
    KsSound *snd = sound_of(native);
    snd->vol = (float) ksc_float(v);
    audio_handle_volume(snd->handle, snd->vol);
}

static KsValue snd_get_rate(KsScript *s, void *native, uint32_t id) {
    (void) s; (void) id;
    return ks_num(sound_of(native)->rate);
}

static void snd_set_rate(KsScript *s, void *native, uint32_t id, KsValue v) {
    (void) s; (void) id;
    KsSound *snd = sound_of(native);
    snd->rate = (float) ksc_float(v);
    audio_handle_rate(snd->handle, snd->rate ? snd->rate : 1.0f);
}

// fade(from = 0, to = 1, ms = 0)
static KsValue snd_fade(KsScript *s, void *native, const KsValue *a, int n) {
    (void) s;
    KsSound *snd = sound_of(native);
    audio_handle_fade(snd->handle, (float) ksc_num_or(ksc_arg(a, n, 0), 0), (float) ksc_num_or(ksc_arg(a, n, 1), 1),
                      (float) ksc_num_or(ksc_arg(a, n, 2), 0));
    return ks_undef();
}

static KsValue snd_mute(KsScript *s, void *native, const KsValue *a, int n) {
    (void) s; (void) a; (void) n;
    audio_handle_mute(sound_of(native)->handle, 1);
    return ks_undef();
}

static KsValue snd_unmute(KsScript *s, void *native, const KsValue *a, int n) {
    (void) s; (void) a; (void) n;
    audio_handle_mute(sound_of(native)->handle, 0);
    return ks_undef();
}

static KsValue snd_play(KsScript *s, void *native, const KsValue *a, int n) {
    (void) s; (void) a; (void) n;
    audio_handle_resume(sound_of(native)->handle);
    return ks_undef();
}

static KsValue snd_stop(KsScript *s, void *native, const KsValue *a, int n) {
    (void) s; (void) a; (void) n;
    // Howl.stop(id): the voice can be played again from the start (play())
    KsSound *snd = sound_of(native);
    audio_handle_pause(snd->handle);
    return ks_undef();
}

static KsValue snd_pause(KsScript *s, void *native, const KsValue *a, int n) {
    (void) s; (void) a; (void) n;
    audio_handle_pause(sound_of(native)->handle);
    return ks_undef();
}

static const KsHostProp SOUND_PROPS[] = {
    {"volume", snd_get_volume, snd_set_volume, NULL, 0},
    {"rate", snd_get_rate, snd_set_rate, NULL, 0},
    {"fade", NULL, NULL, snd_fade, 0},
    {"mute", NULL, NULL, snd_mute, 0},
    {"unmute", NULL, NULL, snd_unmute, 0},
    {"play", NULL, NULL, snd_play, 0},
    {"stop", NULL, NULL, snd_stop, 0},
    {"pause", NULL, NULL, snd_pause, 0},
};
static const KsHostClass SOUND_CLASS = {"sound", SOUND_PROPS, (int) (sizeof(SOUND_PROPS) / sizeof(SOUND_PROPS[0]))};

// parseInt(assetID), then UTILS.isNumber or 0 (strings accepted, VER:1157)
static int asset_id(KsScript *s, const KsValue v) {
    char buf[64];
    ksc_str(s, v, buf, sizeof(buf));
    const char *p = buf;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    char *end;
    const long id = strtol(p, &end, 10);
    return end == p || id < 0 || id > 0x7fffffff ? 0 : (int) id;
}

static KsValue new_sound(KsScript *s, const int handle, const int aid, const float vol, const float rate) {
    if (!handle) return ks_undef();
    // recycle finished sounds first
    for (int i = 0; i < g_sound_count;) {
        if (!audio_handle_active(g_sounds[i]->handle) && g_sound_count >= KSC_SOUND_MAX) {
            ks_host_forget(s, &SOUND_CLASS, (uint32_t) g_sounds[i]->handle);
            free(g_sounds[i]);
            g_sounds[i] = g_sounds[--g_sound_count];
            continue;
        }
        i++;
    }
    if (g_sound_count >= KSC_SOUND_MAX) return ks_undef();
    KsSound *snd = calloc(1, sizeof(KsSound));
    if (!snd) return ks_undef();
    *snd = (KsSound) {handle, aid, vol, rate};
    g_sounds[g_sound_count++] = snd;
    return ks_host_object(s, &SOUND_CLASS, snd, (uint32_t) handle);
}

// play2D(assetID, volume = 1, rate = 0, loop = false); rate p_snd2D 10
static KsValue sound_play_2d(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    if (!ksc_game() || !ks_game_rate(ksc_game(), "p_snd2D", 10, 0)) return ks_undef();
    const int aid = asset_id(s, ksc_arg(a, n, 0));
    const float vol = (float) ksc_num_or(ksc_arg(a, n, 1), 1);
    const float rate = (float) ksc_num_or(ksc_arg(a, n, 2), 0);
    const int loop = ksc_truthy(ksc_arg(a, n, 3));
    char name[32];
    snprintf(name, sizeof(name), "asset:%d", aid);
    const vec3 origin = {0};
    return new_sound(s, audio_handle_play(name, vol, loop, rate > 0 ? rate : 1.0f, 0, origin, 0, 0), aid, vol, rate);
}

// play3D(assetID, volume = 1, x, y, z, rate = 0, loop = false): play3D(id, x, y, z, vol, rate,
// 25, 1, true) (refDistance 25, rolloff 1, otherSoundMlt like every 3D play), play3DL to loop
static KsValue sound_play_3d(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    if (!ksc_game() || !ks_game_rate(ksc_game(), "p_snd3D", 10, 0)) return ks_undef();
    const int aid = asset_id(s, ksc_arg(a, n, 0));
    const float vol = (float) ksc_num_or(ksc_arg(a, n, 1), 1);
    const vec3 pos = {(float) ksc_num_or(ksc_arg(a, n, 2), 0), (float) ksc_num_or(ksc_arg(a, n, 3), 0), (float) ksc_num_or(ksc_arg(a, n, 4), 0)};
    const float rate = (float) ksc_num_or(ksc_arg(a, n, 5), 0);
    const int loop = ksc_truthy(ksc_arg(a, n, 6));
    char name[32];
    snprintf(name, sizeof(name), "asset:%d", aid);
    const float played = loop ? vol : vol * AUDIO_OTHER_SOUND_MLT;
    return new_sound(s, audio_handle_play(name, played, loop, rate > 0 ? rate : 1.0f, 1, pos, 25.0f, 1.0f), aid, vol, rate);
}

// stop(assetID = 0): every playing instance of that asset
static KsValue sound_stop(KsScript *s, void *u, const KsValue *a, int n) {
    (void) u;
    const KsValue v = ksc_arg(a, n, 0);
    const int aid = ksc_is_num(v) ? (int) v.as.num : 0;
    (void) s;
    for (int i = 0; i < g_sound_count; i++) {
        if (g_sounds[i]->aid == aid) audio_handle_stop(g_sounds[i]->handle);
    }
    return ks_undef();
}

void ks_sound_api(KsApiImpl *api) {
    api[KS_API_SOUND_PLAY2D].fn = sound_play_2d;
    api[KS_API_SOUND_PLAY3D].fn = sound_play_3d;
    api[KS_API_SOUND_STOP].fn = sound_stop;
}

// Script sounds stop with the script (2.2 KN rules).
void ks_sound_unload(void) {
    for (int i = 0; i < g_sound_count; i++) {
        audio_handle_stop(g_sounds[i]->handle);
        free(g_sounds[i]);
    }
    g_sound_count = 0;
}
