// The soundscape loop: Krunker v10.0.7's window.changeAmbient (deob-10.0.7.js:227084-227090) on SOUND.play /
// SOUND.stop / SOUND.fadeOut (client/src/audio.c).
#include <sound_ambient.h>
#include <audio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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

// JS truthiness of a map value
static int json_truthy(const cJSON *json) {
    if (!json || cJSON_IsNull(json) || cJSON_IsFalse(json)) return 0;
    if (cJSON_IsNumber(json)) return cJSON_GetNumberValue(json) != 0;
    if (cJSON_IsString(json)) return cJSON_GetStringValue(json)[0] != 0;
    return 1;
}

// parseInt(text): leading blanks, a sign, "0x" hex, decimal digits; SOUND_AMBIENT_NAN without a digit
static int js_parse_int(const char *text) {
    const char *c = text;
    while (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r' || *c == '\v' || *c == '\f') c++;
    int sign = 1;
    if (*c == '-' || *c == '+') sign = *c++ == '-' ? -1 : 1;
    int base = 10;
    if (c[0] == '0' && (c[1] == 'x' || c[1] == 'X')) {
        base = 16;
        c += 2;
    }
    long long value = 0;
    int digits = 0;
    for (;; c++, digits++) {
        const int d = *c >= '0' && *c <= '9' ? *c - '0' : base == 16 && *c >= 'a' && *c <= 'f' ? *c - 'a' + 10
                    : base == 16 && *c >= 'A' && *c <= 'F' ? *c - 'A' + 10 : -1;
        if (d < 0) break;
        if (value < 0x7fffffffLL) value = value * base + d;
    }
    if (!digits) return SOUND_AMBIENT_NAN;
    if (value > 0x7fffffffLL) value = 0x7fffffffLL;
    return (int) (sign * value);
}

int sound_ambient_parse_int(const cJSON *v) {
    if (cJSON_IsNumber(v)) {
        const double d = v->valuedouble;
        if (!isfinite(d)) return SOUND_AMBIENT_NAN;
        const double t = d < 0 ? ceil(d) : floor(d);
        return t > 2147483647.0 ? 2147483647 : t < -2147483647.0 ? -2147483647 : (int) t;
    }
    if (!cJSON_IsString(v) || !v->valuestring) return SOUND_AMBIENT_NAN;
    return js_parse_int(v->valuestring);
}

// 1000 * (v || 0): JS ToNumber of a number, a numeric string or a bool; NaN is falsy, so 0
static float js_fade_ms(const cJSON *v) {
    double x = 0.0;
    if (cJSON_IsNumber(v)) x = v->valuedouble;
    else if (cJSON_IsTrue(v)) x = 1.0;
    else if (cJSON_IsString(v) && v->valuestring) {
        char *end = NULL;
        x = strtod(v->valuestring, &end);
        while (end && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) end++;
        if (!end || *end) x = 0.0; // Number("2s") is NaN
    }
    return isfinite(x) ? (float) (1000.0 * x) : 0.0f;
}

void sound_ambient_map(const cJSON *map, const int mode_amb_ind, int *id, int *custom, float *fade_ms) {
    const cJSON *amb = cJSON_GetObjectItem(map, "ambInd");
    const cJSON *amb_c = cJSON_GetObjectItem(map, "ambIndC");
    *custom = 0;
    if (mode_amb_ind) *id = mode_amb_ind;
    else if (json_truthy(amb)) *id = sound_ambient_parse_int(amb);
    else *id = 1;
    if (json_truthy(amb_c)) {
        *id = sound_ambient_parse_int(amb_c);
        *custom = 1;
    }
    if (fade_ms) *fade_ms = js_fade_ms(cJSON_GetObjectItem(map, "ambFadeTime"));
}

// parseInt(v || 0) of a zone key (MapManager.addEnvironmentZone, deob-10.0.7.js:167125-167127)
static int zone_int(const cJSON *zone, const char *key) {
    const cJSON *v = cJSON_GetObjectItem(zone, key);
    return json_truthy(v) ? sound_ambient_parse_int(v) : 0;
}

// A truthy int: neither 0 nor NaN
static int int_truthy(const int v) { return v != 0 && v != SOUND_AMBIENT_NAN; }

void sound_ambient_environment(const cJSON *zone, const cJSON *map, const int mode_amb_ind) {
    int id, custom;
    float fade_ms;
    if (!zone) { // updateEnvironment(-1): {data: the map's own data}, its keys as the map file has them
        sound_ambient_map(map, mode_amb_ind, &id, &custom, &fade_ms);
        sound_events_change_ambient(id, custom, fade_ms);
        return;
    }
    // addEnvironmentZone: ambInd = `null != ambInd ? parseInt(ambInd || 0) : 1`, ambIndC = parseInt(ambIndC || 0),
    // ambFadeTime = parseInt(ambFadeTime || 0), whole seconds (a 2.5 s slider value fades over 2 s)
    const cJSON *amb = cJSON_GetObjectItem(zone, "ambInd");
    const int zone_amb = amb && !cJSON_IsNull(amb) ? zone_int(zone, "ambInd") : 1;
    const int zone_amb_c = zone_int(zone, "ambIndC");
    const int fade = zone_int(zone, "ambFadeTime");
    // updateEnvironment: `mode.ambInd || data.ambInd || 1`, then a truthy ambIndC as a custom one
    id = mode_amb_ind ? mode_amb_ind : int_truthy(zone_amb) ? zone_amb : 1;
    custom = 0;
    if (int_truthy(zone_amb_c)) {
        id = zone_amb_c;
        custom = 1;
    }
    sound_events_change_ambient(id, custom, int_truthy(fade) ? 1000.0f * (float) fade : 0.0f);
}
