#pragma once
// Client voice chat: Krunker v3.1.1's toggleRecord / voiceChat / addVoiceStream / remSpeaker
// (src/index.js) in C. Protocol and server rules: shared/include/voice.h.
//
// Capture: while the voice key is held, one clip per config.voiceRate (500 ms).
//   native: miniaudio capture device (48 kHz mono) -> Opus (client/lib/opus, linked into every
//           client; Chrome's settings: 60 ms frames, 5 kbit/s) -> WebM like Chrome's
//           MediaRecorder -> "data:audio/ogg; codecs=opus;base64,...".
//   web:    getUserMedia -> a Web Audio tap -> 48 kHz -> the same Opus + WebM encoder (KrunkNative:
//           v3.1.1/v7's MediaRecorder writes per-browser formats, Safari's MP4 plays nowhere else).
// Playback: per-speaker queues (voiceStreamQueues), 2D, volume master * voice * 2 on top of
//   the master gain (Howl volume under Howler.volume), clips over 0.78 s dropped.
//   both:   data URL -> WebM / Ogg Opus (libopus) or WAV / MP3 (miniaudio) -> PCM; the web build
//           hands anything else to the browser's decodeAudioData. A clip that cannot be decoded
//           leaves its queue (KrunkNative: in Krunker it blocked that speaker for good).
//   Both play through audio.c's miniaudio engine.
// Failures are shown: a "Voice unavailable: <reason>" chat line (and stderr), the mic icon's
//   brightness follows the input level, and a silent / dead microphone, micVolume 0 or
//   voiceVolume 0 each get one notice.
//
// Debug environment:
//   KRUNKNATIVE_VOICE_LOG=1        log clips sent / received / played
//   KRUNKNATIVE_VOICE_ECHO=1       also play our own clips as if another player sent them
//   KRUNKNATIVE_VOICE_DUMP=<dir>   write every decoded received clip as <dir>/vc-<n>.wav and every
//                                  sent clip's file as <dir>/sent-<n>.<webm|wav>
//   KRUNKNATIVE_OPUS=0             act as a client without Opus (8 kHz WAV clips, Opus undecodable)
//   KRUNKNATIVE_VOICE_INPUT=<name> capture device by exact name (tests: a virtual microphone),
//                                  overriding the audioInput setting; refused when absent.
//                                  tone:<hz> a generated sine, file:<path> a looped WAV: no device
//                                  (server/tests/voice_e2e.py)
#include <client.h>

// CONTROLS N(voiceKey, down): window.toggleRecord(down) (push to talk; v7 voiceChatTrigger 0: toggle).
void voice_toggle_record(Client *client, int down);
// Per frame (main thread): recorder interval, clips to send, decodes, finished playback.
void voice_update(Client *client);
// NET_PACKET_VOICE (["vc", audio, alias, sid] -> window.voiceChat).
void voice_on_packet(Client *client, const void *payload, uint16_t length);
// NET_PACKET_VOICE_V7 (v7 io_voiceChat: channel, volume, account id; shared/include/voice.h).
void voice_on_packet_v7(Client *client, const void *payload, uint16_t length);
// v7.1.4 voice settings: micQuality, micVolume, voiceChatType, voiceDistance, muteSpec,
// voiceChatTrigger (client_apply_setting).
void voice_apply_setting(const char *key, const char *value);
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
// Status (KrunkNative): the last "Voice unavailable" reason ("" none), the microphone's RMS level
// 0..1 while recording, and the linked libopus ("libopus 1.5.2"; client_bin --voice-info).
const char *voice_last_error(void);
float voice_input_level(void);
const char *voice_opus_version(void);
// audio_fini(): voice sounds live on the audio engine and go before it.
void voice_audio_fini(void);
void voice_fini(void);
