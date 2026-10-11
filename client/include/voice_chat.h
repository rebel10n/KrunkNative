#pragma once
// Client voice chat: Krunker v3.1.1's toggleRecord / voiceChat / addVoiceStream / remSpeaker
// (src/index.js) in C. Protocol and server rules: shared/include/voice.h.
//
// Capture: while the voice key is held, one clip per config.voiceRate (500 ms).
//   native: miniaudio capture device (48 kHz mono) -> Opus (libopus, loaded at run time,
//           Chrome's settings: 60 ms frames, 5 kbit/s) -> WebM like Chrome's MediaRecorder
//           -> "data:audio/ogg; codecs=opus;base64,...". Without libopus: 8 kHz 16-bit WAV
//           ("data:audio/wav;base64,..."), which Krunker's own clients play as well.
//   web:    getUserMedia + MediaRecorder + FileReader.readAsDataURL, exactly v3.1.1's code.
// Playback: per-speaker queues (voiceStreamQueues), 2D, volume master * voice * 2 on top of
//   the master gain (Howl volume under Howler.volume), clips over 0.78 s dropped.
//   native: data URL -> WebM / Ogg Opus (libopus) or WAV / MP3 (miniaudio) -> PCM.
//   web:    the browser's decodeAudioData (what Howler does) -> PCM.
//   Both play through audio.c's miniaudio engine.
//
// Debug environment:
//   KRUNKNATIVE_VOICE_LOG=1        log clips sent / received / played
//   KRUNKNATIVE_VOICE_ECHO=1       also play our own clips as if another player sent them
//   KRUNKNATIVE_VOICE_DUMP=<dir>   write every decoded received clip as <dir>/vc-<n>.wav and every
//                                  sent clip's file as <dir>/sent-<n>.<webm|wav>
//   KRUNKNATIVE_OPUS=<path>|0      libopus to load (0: none, WAV clips)
//   KRUNKNATIVE_VOICE_INPUT=<name> capture device by exact name (tests: a virtual microphone),
//                                  overriding the audioInput setting; refused when absent
#include <client.h>

// CONTROLS N(voiceKey, down): window.toggleRecord(down) (push to talk).
void voice_toggle_record(Client *client, int down);
// Per frame (main thread): recorder interval, clips to send, decodes, finished playback.
void voice_update(Client *client);
// NET_PACKET_VOICE (["vc", audio, alias, sid] -> window.voiceChat).
void voice_on_packet(Client *client, const void *payload, uint16_t length);
// initGame(): voiceStreamQueues = {} (what is playing keeps playing).
void voice_reset_queues(void);
// settings.tmp.audioInput ("default" or a device id from voice_audio_inputs_json()).
void voice_set_audio_input(const char *device_id);
// getAudioInputs(): {"deviceId": "label", ...} JSON object (malloc'd), {"default":"Default"}
// when no device is known.
char *voice_audio_inputs_json(void);
// voiceStreamQueues[name] && voiceStreamQueues[name].length: the nametag speaker icon.
int voice_is_speaking(const char *name);
// The nametag icon ligature ("volume_down" / "volume_up", flipped every 300 ms like the
// overlay's ag/ah counter).
const char *voice_speaker_icon(void);
int voice_recording(void); // cB
// audio_fini(): voice sounds live on the audio engine and go before it.
void voice_audio_fini(void);
void voice_fini(void);
