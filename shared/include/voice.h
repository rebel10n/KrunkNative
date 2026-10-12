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
//
// v7.1.4 (player-viewer WP-E part 2b, src/index.js io_voiceChat / toggleRecord, server/index.ts
// "vc"): the clip carries the sender's text & voice channel, mic volume and mic quality; the
// relay adds the channel, volume, isFinal and the account id, and the receiver plays a clip only
// when its channel is -1 or its own. KrunkNative ids (shared/include/social_v7.h):
//   NET_PACKET_VOICE_V7 (C->S): chunk.id = clip counter, data = "<channel> <volume> <quality>\n"
//                               + the data URL.
//   NET_PACKET_VOICE_V7 (S->C): chunk.id = speaker sid, data = "<channel> <volume> <isFinal>
//                               <accountId> <alias bytes>\n" + alias + the data URL.
// The headers are ASCII so that a reassembled message (NUL-terminated) has no inner NULs.
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

// v7.1.4
#define VOICE_V7_MAX_DATA 750000u     // server: audioData.length > 750_000 is dropped (the chunk assembly
                                      // already stops at NET_SOCIAL_MAX_MESSAGE, 512 KiB)
#define VOICE_V7_DATA_PREFIX "data:audio/" // server: !audioData.startsWith("data:audio/") is dropped
#define VOICE_MIC_QUALITY_DEFAULT 6000 // settings micQuality: MediaRecorder bitsPerSecond
#define VOICE_V7_HEADER_MAX 96

typedef struct {
    int channel;   // CHAT_CHANNEL_* of the sender (vLN02023)
    float volume;  // micVolume 0..1 (the server clamps it)
    int quality;   // micQuality (bits per second; the server ignores it, like v7's)
} VoiceV7Send;

typedef struct {
    int channel;    // -1 every channel, else the channel it was spoken on
    float volume;   // the speaker's micVolume, 0..1
    int is_final;   // isFinal (v7's server always sends false)
    int account_id; // speakerAccountId (mute_voice), 0 guest
} VoiceV7Meta;

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

// The S->C NET_PACKET_VOICE body for `audio`: [alias length u8][alias bytes][data URL] (malloc'd,
// *length set). NULL on OOM.
char *voice_relay_message(const char *alias, const char *audio, size_t *length);

// Client: a completed NET_PACKET_VOICE message (social_assembly_push output, chunk id = sid)
// -> alias and audio (pointing into `msg`). Returns 0 when malformed.
int voice_parse_relay(const char *msg, char *alias, size_t alias_size, const char **audio);

// ---- v7.1.4 framing

// Client: one recorded clip -> NET_PACKET_VOICE_V7 chunks. 0 when not sendable or a send failed.
int voice_send_clip_v7(const char *audio, size_t length, uint32_t seq, const VoiceV7Send *meta, SocialSendFn send, void *user);
// Server: a completed C->S NET_PACKET_VOICE_V7 message -> its header and the data URL (pointing
// into `msg`). 0 when malformed or not a clip v7's server relays ("data:audio/", <= 750 000).
// The volume comes back clamped to 0..1 and a non-integer channel as -1 (integer(args[1], -1)).
int voice_parse_send_v7(const char *msg, VoiceV7Send *meta, const char **audio);
// Server: the S->C message body for `audio` (malloc'd, *length set). NULL on OOM.
char *voice_relay_v7_message(const VoiceV7Meta *meta, const char *alias, const char *audio, size_t *length);
// Client: a completed S->C NET_PACKET_VOICE_V7 message -> header, alias and audio. 0 when malformed.
int voice_parse_relay_v7(const char *msg, VoiceV7Meta *meta, char *alias, size_t alias_size, const char **audio);
