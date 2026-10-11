#pragma once
// Krunker v3.1.1 voice chat protocol and relay (shared by server_bin and the offline game).
//
// v3.1.1 (src/index.js toggleRecord / voiceChat, server/packetHandlers.ts io_vc):
//   client: while the voice key is held, a MediaRecorder (bitsPerSecond 6000) is stopped and
//     restarted every config.voiceRate (500 ms); every stop yields one standalone file which
//     FileReader.readAsDataURL turns into "data:audio/ogg; codecs=opus;base64,<file>" and
//     io.send("vc", dataUrl). Chrome writes WebM (A_OPUS, 48 kHz mono, 60 ms SILK frames,
//     ~5 kbit/s: the 6000 bps budget gets the audio share of MediaRecorder's split), Firefox
//     Ogg Opus; the "audio/ogg" label is the Blob's type either way.
//   server: io_vc(ctx, audio) drops it unless the socket has a player (spectators have none)
//     and audio is a non-empty string of at most 256 KiB, then sends ["vc", audio, alias, sid]
//     to every other socket of the lobby: no team, mode, alive, rate or mute rules.
//   receiver: voiceChat(audio, alias, sid) -> addVoiceStream (Howler decodes the data URL).
//
// KrunkNative wire: the same strings, cut into NetSocialChunk pieces (shared/include/social.h)
//   NET_PACKET_VOICE_SEND (C->S): chunk.id = sender's clip counter, data = the data URL.
//   NET_PACKET_VOICE      (S->C): chunk.id = speaker sid (uid), data = [alias length u8]
//                                 [alias bytes][data URL].
#include <shared.h>
#include <social.h>

// config.js
#define VOICE_CHAT_INTERVAL_MS 2000 // config.voiceChatInterval (defined, read by nothing in v3.1.1)
#define VOICE_CHAT_MAX_LENGTH 0.65f // config.voiceChatMaxLength: clips longer than 1.2x are dropped
#define VOICE_RATE_MS 500           // config.voiceRate: recorder restart interval
#define VOICE_DELAY_MS 200          // config.voiceDelay (defined, read by nothing in v3.1.1)
#define VOICE_BITS_PER_SECOND 6000  // new MediaRecorder(stream, {bitsPerSecond: 6000})
#define VOICE_OPUS_BITRATE 5000     // what Chrome's encoder gets for it (MediaRecorder.audioBitsPerSecond)
#define VOICE_SAMPLE_RATE 48000     // Opus / getUserMedia track rate
#define VOICE_FRAME_MS 60           // Chrome's Opus frame (kOpusPreferredBufferDurationMs)
#define VOICE_MAX_DATA (256u * 1024u) // io_vc: audio.length > 256 * 1024 is dropped
#define VOICE_ALIAS_MAX 255         // alias bytes carried by NET_PACKET_VOICE (u8 length)
#define VOICE_DATA_URL_PREFIX "data:audio/ogg; codecs=opus;base64,"

typedef struct {
    int sid;           // Krunker player.sid (KrunkNative uid)
    const char *alias; // player.alias
    int has_player;    // ctx.player: joined the match (not a spectator)
    int peer;          // index of the speaker's own connection in the peer list (-1 none)
} VoiceSpeaker;

// Sends one packet to peer `peer`; returns 0 when it failed (the rest of that clip is skipped).
typedef int (*VoicePeerSendFn)(void *user, int peer, NetPacketType type, const void *payload, uint16_t length);

// Client: one recorded clip -> NET_PACKET_VOICE_SEND chunks. Returns 0 when it is not a
// sendable clip (empty / too large) or a send failed.
int voice_send_clip(const char *audio, size_t length, uint32_t seq, SocialSendFn send, void *user);

// io_vc: feeds one NET_PACKET_VOICE_SEND packet of `speaker`'s connection (`assembly` is that
// connection's). When it completes a clip that passes io_vc's checks, the clip goes to every
// peer in [0, peer_count) but the speaker's. Returns the peers it reached (0 while incomplete
// or dropped), -1 on a malformed chunk.
int voice_relay(SocialAssembly *assembly, const void *payload, size_t length, const VoiceSpeaker *speaker,
                int peer_count, VoicePeerSendFn send, void *user);

// Client: a completed NET_PACKET_VOICE message (social_assembly_push output, chunk id = sid)
// -> alias and audio (pointing into `msg`). Returns 0 when malformed.
int voice_parse_relay(const char *msg, char *alias, size_t alias_size, const char **audio);
