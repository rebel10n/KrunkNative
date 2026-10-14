#!/bin/bash
# Nettspend class sound clips (doc/v7/decisions.md "Nettspend"): every clip is synthesized here
# from scratch with espeak-ng (formant TTS, no recorded voice), SoX signal generators and ffmpeg
# (libmp3lame). Nothing is sampled, downloaded or transcribed from any recording, song or
# performance: the voice is espeak-ng's own synthetic "en-us" voice reading plain text, the rest
# is noise and oscillators. Re-run to regenerate byte-for-byte equivalent clips (the LAME encoder
# is deterministic for the same input; `sox -R` seeds its noise generators the same way each run).
#
#   tools/audio/nettspend_clips.sh [out dir]      (default assets/sound)
#
# Output (44.1 kHz mono MP3 128 kb/s like Krunker's own reload_1.mp3; loaded by client/src/audio.c
# as assets/sound/<name>.mp3, played by client/src/nettspend_audio.c):
#   kn_drank_reload.mp3   the "pour up" reload: a synthetic scream (0.5 s), then the voice line
#                         "drank, drank, drank" (0.9 s): 1.46 s, the Drank's reload time
#   kn_drank_pour.mp3     the cup being poured: filtered pink noise with a glug tone (~1.2 s)
#   kn_drank_splash.mp3   one splash of the stream (0.22 s): the Drank's Weapon.sound, played at
#                         most every 220 ms while spraying (sound_events.c, like the Piss's spray)
#   kn_money_throw.mp3    Money Spread's cash throw: a whoosh and a paper flutter (~0.5 s)
#   kn_laced_ambience.mp3 the "laced" state's ambience: a detuned harmonic drone that loops
#                         (6 s; every component has a whole number of cycles in the loop)
set -euo pipefail

OUT=${1:-assets/sound}
mkdir -p "$OUT"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/kn-nettspend-clips.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
RATE=44100

for tool in espeak-ng sox ffmpeg; do
    command -v "$tool" > /dev/null || { echo "$0: needs $tool" >&2; exit 1; }
done

# WAV -> the game's MP3 (mono, 44.1 kHz, 128 kb/s CBR; -3 dB peak headroom first)
encode() { # <in.wav> <name>
    sox -R "$1" -r $RATE -c 1 -b 16 "$TMP/$2.norm.wav" norm -3
    ffmpeg -y -loglevel error -i "$TMP/$2.norm.wav" -codec:a libmp3lame -b:a 128k -ar $RATE -ac 1 "$OUT/$2.mp3"
    echo "$OUT/$2.mp3: $(soxi -D "$TMP/$2.norm.wav") s"
}

# ---- kn_drank_reload: scream + "DRANK! DRANK! DRANK!" ----------------------------------------
# The scream: espeak-ng shouting a long "aaaah" (pitch 99, loud), then SoX pitches it up, adds a
# fast vibrato, overdrives it and rolls off the top.
espeak-ng -v en-us+m3 -p 99 -s 95 -a 200 "aaaaaaaaaah" -w "$TMP/scream.raw.wav"
sox -R "$TMP/scream.raw.wav" -r $RATE -c 1 "$TMP/scream.wav" \
    gain -8 pitch 450 tremolo 9 55 overdrive 22 35 highpass 220 lowpass 5500 \
    trim 0 0.5 fade h 0.01 0.5 0.1 norm -2
# The line: the same synthetic voice, lower and fast (260 wpm, the commas are its only pauses),
# a touch of overdrive and a 45 ms slapback so it reads as shouted in a room; cut at 0.9 s.
espeak-ng -v en-us+m3 -p 35 -s 260 -a 160 -g 1 "drank, drank, drank" -w "$TMP/line.raw.wav"
sox -R "$TMP/line.raw.wav" -r $RATE -c 1 "$TMP/line.wav" \
    gain -8 pitch -150 overdrive 12 25 bass +5 echo 0.85 0.8 45 0.3 trim 0 0.9 fade t 0 0.9 0.06 norm -2
# 60 ms of silence between the scream and the line
sox -R -n -r $RATE -c 1 "$TMP/gap.wav" trim 0 0.06
sox "$TMP/scream.wav" "$TMP/gap.wav" "$TMP/line.wav" "$TMP/reload.wav"
encode "$TMP/reload.wav" kn_drank_reload

# ---- kn_drank_pour: a cup being poured -------------------------------------------------------
# Pink noise band-limited to a liquid hiss with a slow swell, plus a "glug" tone that rises as
# the cup fills (sine 150 -> 420 Hz under a 9 Hz tremolo), both faded in and out.
sox -R -n -r $RATE -c 1 "$TMP/pour_noise.wav" synth 1.2 pinknoise gain -8 \
    highpass 350 lowpass 2200 tremolo 11 65 fade t 0.08 1.2 0.35 gain +2
sox -R -n -r $RATE -c 1 "$TMP/pour_glug.wav" synth 1.2 sine 150:420 \
    tremolo 9 85 lowpass 900 fade t 0.15 1.2 0.4 gain -9
sox -m "$TMP/pour_noise.wav" "$TMP/pour_glug.wav" "$TMP/pour.wav"
encode "$TMP/pour.wav" kn_drank_pour

# ---- kn_drank_splash: one splash of the stream -------------------------------------------------
# A short burst of the pour's liquid hiss (pink noise 400-3000 Hz) with a 15 ms attack and a quick
# decay, plus a low thump (sine 120 -> 60 Hz) so it reads as liquid hitting something.
sox -R -n -r $RATE -c 1 "$TMP/splash_noise.wav" synth 0.22 pinknoise gain -8 highpass 400 lowpass 3000 fade t 0.015 0.22 0.17 gain +1
sox -R -n -r $RATE -c 1 "$TMP/splash_thump.wav" synth 0.12 sine 120:60 fade t 0.005 0.12 0.1 gain -6
sox -m "$TMP/splash_noise.wav" "$TMP/splash_thump.wav" "$TMP/splash.wav"
encode "$TMP/splash.wav" kn_drank_splash

# ---- kn_money_throw: a fan of bills thrown ----------------------------------------------------
# A whoosh (white noise through a 1.2 kHz band, fast in / slow out) under a paper flutter (noise
# above 2.5 kHz chopped at 28 Hz) and a snap at the start.
sox -R -n -r $RATE -c 1 "$TMP/whoosh.wav" synth 0.5 whitenoise gain -8 bandpass 1200 2q fade t 0.03 0.5 0.4 gain -2
sox -R -n -r $RATE -c 1 "$TMP/flutter.wav" synth 0.45 whitenoise gain -8 highpass 2500 tremolo 28 95 fade t 0.02 0.45 0.25 gain -4
sox -R -n -r $RATE -c 1 "$TMP/snap.wav" synth 0.05 whitenoise gain -8 fade 0 0.05 0.045 gain +2
sox -m "$TMP/whoosh.wav" "$TMP/flutter.wav" "$TMP/snap.wav" "$TMP/money.wav"
encode "$TMP/money.wav" kn_money_throw

# ---- kn_laced_ambience: the laced drone (loops) ---------------------------------------------
# Harmonics of 55 Hz (55 110 165 220 275 Hz) with a 0.5 Hz tremolo: 6 s holds 330 / 660 / 990 /
# 1320 / 1650 cycles and 3 tremolo cycles, so the loop point is continuous. The detune is a 221 Hz
# partner of the 220 (a 1 Hz beat, 6 cycles) and the top is rolled off.
sox -R -n -r $RATE -c 1 "$TMP/drone.wav" synth 6 sine 55 sine mix 110 sine mix 165 sine mix 220 sine mix 221 sine mix 275 \
    lowpass 600 tremolo 0.5 45 gain -3
encode "$TMP/drone.wav" kn_laced_ambience
