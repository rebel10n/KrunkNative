// Client voice chat (client/include/voice_chat.h): Krunker v3.1.1 src/index.js
//   toggleRecord   push to talk: getUserMedia, MediaRecorder restarted every voiceRate, each
//                  stop -> readAsDataURL -> io.send("vc", dataUrl); #voiceDisplay opacity
//                  (KrunkNative records the clips itself on both platforms: libopus + WebM)
//   voiceChat      "vc" from the server: dropped while the voice volume is 0
//   addVoiceStream one Howl per clip in voiceStreamQueues[alias]; plays when loaded, first in
//                  its queue and its player is known; longer than voiceChatMaxLength * 1.2: gone
//   remSpeaker     a clip ended: next one in the queue plays
// v7.1.4 (src/index.js:2221-2358, player-viewer WP-E part 2b) on top: the clip carries the text &
// voice channel, micVolume and micQuality ("vc", dataUrl, channel, micVolume, micQuality); a
// received clip plays when its channel is -1 or ours, scaled by the speaker's micVolume, unless
// its account is in mute_voice or muteSpec drops it; voiceChatType 1 places it at the speaker's
// head (refDistance voiceDistance * 5, rolloff 1); voiceChatTrigger 0 makes the key a toggle.
// The protocol / server side is shared/src/voice.c.
#include <voice_chat.h>
#include "klog.h"
#include <hud_social.h>      // social_server_v7, mute_voice
#include <player_accounts.h> // the account id of an older server's speaker (mute_voice)
#include <player_names.h>    // the speaker's name as the notice shows it (Hide player names)
#include <streamer.h>
#include <voice.h>
#include <audio.h>
#include <html_dom.h>
#include <html_ui.h>
#include <spectate.h>
#include <ui_logic.h> // ui_scenario_active, ui_text_input_focused
#include "audio_miniaudio.h"
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define VOICE_FRAME_SAMPLES (VOICE_SAMPLE_RATE * VOICE_FRAME_MS / 1000) // 2880
#define VOICE_MAX_INSTANCES 32
#define VOICE_MAX_SPEAKERS 64
#define VOICE_MAX_QUEUE 1024           // per speaker (v3.1.1: unbounded)
#define VOICE_NAME_MAX 64
#define VOICE_CAPTURE_FRAMES (VOICE_SAMPLE_RATE * 2) // ring buffer: 2 s
#define VOICE_FALLBACK_RATE 8000       // WAV clips without libopus
#define VOICE_ICON_MS 300.0            // overlay ah: speaker icon frame

// v7.1.4 voice settings (settings.js audio tab; client_apply_setting -> voice_apply_setting)
static struct {
    int mic_quality;      // micQuality: MediaRecorder bitsPerSecond
    float mic_volume;     // micVolume: sent with every clip
    int chat_type;        // voiceChatType: 0 default, 1 spatial / proximity
    float distance;       // voiceDistance: (slider || 1) * 5
    int mute_spec;        // muteSpec
    int trigger;          // voiceChatTrigger: 0 press (toggle), 1 continuous (hold)
} g_vset = {VOICE_MIC_QUALITY_DEFAULT, 1.0f, 0, 25.0f, 0, 1};

static int voice_log_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("KRUNKNATIVE_VOICE_LOG");
        cached = v && *v && strcmp(v, "0") != 0;
#ifdef __EMSCRIPTEN__
        // the web has no environment: ?voicelog=1
        if (!cached) cached = EM_ASM_INT({ try { return new URLSearchParams(location.search).get('voicelog') == '1' ? 1 : 0; } catch (e) { return 0; } });
#endif
    }
    return cached;
}

static void voice_log(const char *fmt, ...) {
    if (!voice_log_enabled()) return;
    va_list args;
    va_start(args, fmt);
    KN_LOG_VERBOSE2("[voice] t=%.3f ", glfwGetTime());
    vprintf(fmt, args);
    printf("\n");
    fflush(stdout);
    va_end(args);
}

// ---------------------------------------------------------------- byte buffer / base64

typedef struct {
    uint8_t *data;
    size_t len, cap;
    int failed;
} VoiceBuf;

static void vbuf_put(VoiceBuf *b, const void *data, const size_t n) {
    if (b->failed || !n) return;
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n) cap *= 2;
        uint8_t *grown = realloc(b->data, cap);
        if (!grown) {
            b->failed = 1;
            return;
        }
        b->data = grown;
        b->cap = cap;
    }
    memcpy(b->data + b->len, data, n);
    b->len += n;
}

static void vbuf_u8(VoiceBuf *b, const uint8_t v) {
    vbuf_put(b, &v, 1);
}

static void vbuf_le(VoiceBuf *b, const uint32_t v, const int bytes) {
    for (int i = 0; i < bytes; i++) vbuf_u8(b, (uint8_t) (v >> (8 * i)));
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// FileReader.readAsDataURL: prefix + base64(data), NUL-terminated.
static char *b64_data_url(const char *prefix, const uint8_t *data, const size_t n, size_t *out_len) {
    const size_t plen = strlen(prefix);
    const size_t len = plen + (n + 2) / 3 * 4;
    char *out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, prefix, plen);
    char *o = out + plen;
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        const uint32_t v = (uint32_t) data[i] << 16 | (uint32_t) data[i + 1] << 8 | data[i + 2];
        *o++ = B64[v >> 18 & 63];
        *o++ = B64[v >> 12 & 63];
        *o++ = B64[v >> 6 & 63];
        *o++ = B64[v & 63];
    }
    if (i < n) {
        const uint32_t v = (uint32_t) data[i] << 16 | (i + 1 < n ? (uint32_t) data[i + 1] << 8 : 0);
        *o++ = B64[v >> 18 & 63];
        *o++ = B64[v >> 12 & 63];
        *o++ = i + 1 < n ? B64[v >> 6 & 63] : '=';
        *o++ = '=';
    }
    *o = 0;
    if (out_len) *out_len = len;
    return out;
}

static int b64_value(const int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

// "data:<type>;...;base64,<payload>" -> payload bytes (malloc'd). Only base64 data URLs carry
// audio in practice (FileReader.readAsDataURL always writes them).
static uint8_t *data_url_decode(const char *url, size_t *out_len) {
    if (!url || strncmp(url, "data:", 5) != 0) return NULL;
    const char *comma = strchr(url, ',');
    if (!comma) return NULL;
    int base64 = 0;
    for (const char *p = url; p + 7 <= comma; p++) {
        if (!strncmp(p, ";base64", 7)) base64 = 1;
    }
    if (!base64) return NULL;
    const char *s = comma + 1;
    const size_t n = strlen(s);
    uint8_t *out = malloc(n / 4 * 3 + 3);
    if (!out) return NULL;
    size_t len = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '=') break;
        const int v = b64_value((unsigned char) s[i]);
        if (v < 0) continue; // whitespace
        acc = acc << 6 | (uint32_t) v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[len++] = (uint8_t) (acc >> bits);
        }
    }
    *out_len = len;
    return out;
}

// ---------------------------------------------------------------- libopus (vendored, linked)

// KrunkNative fix: libopus used to be dlopen()ed, and the Windows / macOS packages (and Linux
// systems without libopus0) had none. Those clients sent 8 kHz WAV and silently dropped every
// Opus clip, which is every clip from a browser (MediaRecorder) and from any client that had
// libopus: they heard nobody. client/lib/opus is now compiled into every client.
#include <opus.h>

static struct {
    OpusEncoder *(*encoder_create)(int32_t fs, int channels, int application, int *error);
    int (*encoder_ctl)(OpusEncoder *, int request, ...);
    int32_t (*encode_float)(OpusEncoder *, const float *pcm, int frame_size, unsigned char *data, int32_t max_bytes);
    void (*encoder_destroy)(OpusEncoder *);
    OpusDecoder *(*decoder_create)(int32_t fs, int channels, int *error);
    int (*decode_float)(OpusDecoder *, const unsigned char *data, int32_t len, float *pcm, int frame_size, int fec);
    void (*decoder_destroy)(OpusDecoder *);
} g_opus = {opus_encoder_create, opus_encoder_ctl,  opus_encode_float, opus_encoder_destroy,
            opus_decoder_create, opus_decode_float, opus_decoder_destroy};

// KRUNKNATIVE_OPUS=0 (tests): behave like a client without Opus (WAV clips, Opus clips undecodable).
static int opus_available(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *forced = getenv("KRUNKNATIVE_OPUS");
        cached = !(forced && !strcmp(forced, "0"));
    }
    return cached;
}

const char *voice_opus_version(void) {
    return opus_get_version_string();
}

// ---------------------------------------------------------------- WebM (Matroska) writer

#define EBML_HEADER 0x1A45DFA3u
#define MKV_SEGMENT 0x18538067u
#define MKV_INFO 0x1549A966u
#define MKV_TRACKS 0x1654AE6Bu
#define MKV_TRACK_ENTRY 0xAEu
#define MKV_CODEC_ID 0x86u
#define MKV_CODEC_PRIVATE 0x63A2u
#define MKV_AUDIO 0xE1u
#define MKV_CHANNELS 0x9Fu
#define MKV_CLUSTER 0x1F43B675u
#define MKV_TIMECODE 0xE7u
#define MKV_SIMPLE_BLOCK 0xA3u
#define MKV_BLOCK_GROUP 0xA0u
#define MKV_BLOCK 0xA1u

static void ebml_id(VoiceBuf *b, const uint32_t id) {
    int bytes = id > 0xFFFFFF ? 4 : id > 0xFFFF ? 3 : id > 0xFF ? 2 : 1;
    while (bytes--) vbuf_u8(b, (uint8_t) (id >> (8 * bytes)));
}

// Data size as an EBML vint: `width` bytes (0: the shortest).
static void ebml_size(VoiceBuf *b, const uint64_t size, int width) {
    if (!width) {
        width = 1;
        while (width < 8 && size >= ((uint64_t) 1 << (7 * width)) - 1) width++;
    }
    for (int i = width - 1; i >= 0; i--) {
        uint8_t byte = (uint8_t) (size >> (8 * i));
        if (i == width - 1) byte |= (uint8_t) (0x80 >> (width - 1));
        vbuf_u8(b, byte);
    }
}

static void ebml_bytes(VoiceBuf *b, const uint32_t id, const void *data, const size_t n, const int size_width) {
    ebml_id(b, id);
    ebml_size(b, n, size_width);
    vbuf_put(b, data, n);
}

static void ebml_uint(VoiceBuf *b, const uint32_t id, const uint64_t v) {
    uint8_t bytes[8];
    int n = 1;
    while (n < 8 && v >> (8 * n)) n++;
    for (int i = 0; i < n; i++) bytes[i] = (uint8_t) (v >> (8 * (n - 1 - i)));
    ebml_bytes(b, id, bytes, (size_t) n, 0);
}

static void ebml_float(VoiceBuf *b, const uint32_t id, const float v) {
    uint32_t bits;
    memcpy(&bits, &v, 4);
    const uint8_t bytes[4] = {(uint8_t) (bits >> 24), (uint8_t) (bits >> 16), (uint8_t) (bits >> 8), (uint8_t) bits};
    ebml_bytes(b, id, bytes, 4, 0);
}

static void ebml_string(VoiceBuf *b, const uint32_t id, const char *s) {
    ebml_bytes(b, id, s, strlen(s), 0);
}

static void ebml_master(VoiceBuf *b, const uint32_t id, const VoiceBuf *body, const int size_width) {
    ebml_bytes(b, id, body->data, body->len, size_width);
}

static void ebml_free(VoiceBuf *b) {
    free(b->data);
    memset(b, 0, sizeof(*b));
}

// One clip the way Chrome's MediaRecorder writes it on stop(): EBML header, Segment with
// Info (Duration = last block time), one A_OPUS track (OpusHead, pre-skip 0), one Cluster of
// 60 ms SimpleBlocks. (Chrome also writes SeekHead / Void / Cues, which players ignore.)
static int webm_write(VoiceBuf *out, const uint8_t *const *packets, const int *sizes, const int count) {
    VoiceBuf header = {0}, info = {0}, audio = {0}, entry = {0}, tracks = {0}, cluster = {0}, segment = {0};

    ebml_uint(&header, 0x4286, 1);  // EBMLVersion
    ebml_uint(&header, 0x42F7, 1);  // EBMLReadVersion
    ebml_uint(&header, 0x42F2, 4);  // EBMLMaxIDLength
    ebml_uint(&header, 0x42F3, 8);  // EBMLMaxSizeLength
    ebml_string(&header, 0x4282, "webm");
    ebml_uint(&header, 0x4287, 4);  // DocTypeVersion
    ebml_uint(&header, 0x4285, 2);  // DocTypeReadVersion
    ebml_master(out, EBML_HEADER, &header, 0);

    ebml_uint(&info, 0x2AD7B1, 1000000); // TimecodeScale: 1 ms
    ebml_float(&info, 0x4489, (float) (count > 0 ? (count - 1) * VOICE_FRAME_MS : 0)); // Duration
    ebml_string(&info, 0x4D80, "Krunker Box Office"); // MuxingApp
    ebml_string(&info, 0x5741, "Krunker Box Office"); // WritingApp

    VoiceBuf opus_head = {0};
    vbuf_put(&opus_head, "OpusHead", 8);
    vbuf_u8(&opus_head, 1);                      // version
    vbuf_u8(&opus_head, 1);                      // channels
    vbuf_le(&opus_head, 0, 2);                   // pre-skip (Chrome writes 0)
    vbuf_le(&opus_head, VOICE_SAMPLE_RATE, 4);   // input sample rate
    vbuf_le(&opus_head, 0, 2);                   // output gain
    vbuf_u8(&opus_head, 0);                      // channel mapping family

    ebml_float(&audio, 0xB5, (float) VOICE_SAMPLE_RATE); // SamplingFrequency
    ebml_uint(&audio, MKV_CHANNELS, 1);
    ebml_uint(&audio, 0x6264, 32);                      // BitDepth
    ebml_uint(&entry, 0xD7, 1);                         // TrackNumber
    ebml_uint(&entry, 0x73C5, ((uint64_t) rand() << 24 ^ (uint64_t) rand()) & 0xFFFFFFFFFFFFFFull); // TrackUID
    ebml_uint(&entry, 0x83, 2);                         // TrackType: audio
    ebml_string(&entry, MKV_CODEC_ID, "A_OPUS");
    ebml_bytes(&entry, MKV_CODEC_PRIVATE, opus_head.data, opus_head.len, 0);
    ebml_master(&entry, MKV_AUDIO, &audio, 0);
    ebml_master(&tracks, MKV_TRACK_ENTRY, &entry, 0);

    ebml_uint(&cluster, MKV_TIMECODE, 0);
    for (int i = 0; i < count; i++) {
        const int tc = i * VOICE_FRAME_MS;
        const uint8_t block_header[4] = {0x81, (uint8_t) (tc >> 8), (uint8_t) tc, 0x80}; // track 1, keyframe
        ebml_id(&cluster, MKV_SIMPLE_BLOCK);
        ebml_size(&cluster, 4 + (uint64_t) sizes[i], 0);
        vbuf_put(&cluster, block_header, 4);
        vbuf_put(&cluster, packets[i], (size_t) sizes[i]);
    }

    ebml_master(&segment, MKV_INFO, &info, 0);
    ebml_master(&segment, MKV_TRACKS, &tracks, 0);
    ebml_master(&segment, MKV_CLUSTER, &cluster, 8);
    ebml_master(out, MKV_SEGMENT, &segment, 8);

    const int failed = header.failed || info.failed || audio.failed || entry.failed || tracks.failed ||
        cluster.failed || segment.failed || opus_head.failed || out->failed;
    ebml_free(&header);
    ebml_free(&info);
    ebml_free(&audio);
    ebml_free(&entry);
    ebml_free(&tracks);
    ebml_free(&cluster);
    ebml_free(&segment);
    ebml_free(&opus_head);
    return !failed;
}

// ---------------------------------------------------------------- Opus clip decoding

typedef struct {
    float *pcm;          // interleaved
    uint64_t frames;
    uint32_t channels;
    uint32_t rate;
} VoicePcm;

typedef struct {
    OpusDecoder *decoder;
    int channels;
    int pre_skip;
    VoicePcm out;
    size_t cap; // frames
    int failed;
} OpusSink;

static void opus_sink_head(OpusSink *sink, const uint8_t *head, const size_t n) {
    if (n < 19 || memcmp(head, "OpusHead", 8) != 0) return;
    sink->channels = head[9] >= 2 ? 2 : 1;
    sink->pre_skip = head[10] | head[11] << 8;
}

static void opus_sink_packet(OpusSink *sink, const uint8_t *data, const size_t n) {
    if (sink->failed || !n) return;
    if (!sink->decoder) {
        int error = 0;
        if (!sink->channels) sink->channels = 1;
        sink->decoder = g_opus.decoder_create(VOICE_SAMPLE_RATE, sink->channels, &error);
        if (!sink->decoder) {
            sink->failed = 1;
            return;
        }
        sink->out.channels = (uint32_t) sink->channels;
        sink->out.rate = VOICE_SAMPLE_RATE;
    }
    const size_t max_frames = 5760; // 120 ms, the longest Opus packet
    if (sink->out.frames + max_frames > sink->cap) {
        size_t cap = sink->cap ? sink->cap * 2 : VOICE_SAMPLE_RATE;
        while (cap < sink->out.frames + max_frames) cap *= 2;
        float *grown = realloc(sink->out.pcm, cap * (size_t) sink->channels * sizeof(float));
        if (!grown) {
            sink->failed = 1;
            return;
        }
        sink->out.pcm = grown;
        sink->cap = cap;
    }
    const int got = g_opus.decode_float(sink->decoder, data, (int32_t) n, sink->out.pcm + sink->out.frames * (size_t) sink->channels,
                                        (int) max_frames, 0);
    if (got < 0) {
        sink->failed = 1; // a corrupt packet: decodeAudioData rejects the whole file
        return;
    }
    sink->out.frames += (uint64_t) got;
}

static int opus_sink_finish(OpusSink *sink, VoicePcm *out) {
    if (sink->decoder) g_opus.decoder_destroy(sink->decoder);
    sink->decoder = NULL;
    if (sink->failed || !sink->out.pcm) {
        free(sink->out.pcm);
        return 0;
    }
    const uint64_t skip = (uint64_t) sink->pre_skip < sink->out.frames ? (uint64_t) sink->pre_skip : sink->out.frames;
    if (skip) {
        memmove(sink->out.pcm, sink->out.pcm + skip * sink->out.channels, (size_t) ((sink->out.frames - skip) * sink->out.channels * sizeof(float)));
        sink->out.frames -= skip;
    }
    *out = sink->out;
    return 1;
}

static int ebml_read_vint(const uint8_t *p, const uint8_t *end, uint64_t *value, int *len, const int keep_marker, int *unknown) {
    if (p >= end || !*p) return 0;
    int n = 1;
    uint8_t mask = 0x80;
    while (!(*p & mask)) {
        mask >>= 1;
        n++;
    }
    if (p + n > end) return 0;
    uint64_t v = keep_marker ? *p : (uint64_t) (*p & (mask - 1));
    int all_ones = (*p & (mask - 1)) == mask - 1;
    for (int i = 1; i < n; i++) {
        v = v << 8 | p[i];
        if (p[i] != 0xFF) all_ones = 0;
    }
    *value = v;
    *len = n;
    if (unknown) *unknown = !keep_marker && all_ones;
    return 1;
}

// One (Simple)Block of track 1 without lacing (what MediaRecorder writes).
static void webm_block(OpusSink *sink, const uint8_t *p, const uint8_t *end) {
    uint64_t track;
    int len;
    if (!ebml_read_vint(p, end, &track, &len, 0, NULL) || p + len + 3 > end) return;
    const uint8_t flags = p[len + 2];
    if ((flags & 0x06) != 0) return; // laced: not produced by any recorder Krunker meets
    opus_sink_packet(sink, p + len + 3, (size_t) (end - (p + len + 3)));
}

static void webm_walk(OpusSink *sink, const uint8_t *p, const uint8_t *end, int *is_opus) {
    while (p < end && !sink->failed) {
        uint64_t id, size;
        int id_len, size_len, unknown;
        if (!ebml_read_vint(p, end, &id, &id_len, 1, NULL)) return;
        if (!ebml_read_vint(p + id_len, end, &size, &size_len, 0, &unknown)) return;
        const uint8_t *body = p + id_len + size_len;
        const uint8_t *body_end = unknown || size > (uint64_t) (end - body) ? end : body + size;
        switch ((uint32_t) id) {
            case MKV_SEGMENT:
            case MKV_TRACKS:
            case MKV_TRACK_ENTRY:
            case MKV_CLUSTER:
            case MKV_BLOCK_GROUP:
                webm_walk(sink, body, body_end, is_opus);
                break;
            case MKV_CODEC_ID:
                *is_opus = body_end - body == 6 && !memcmp(body, "A_OPUS", 6);
                break;
            case MKV_CODEC_PRIVATE:
                opus_sink_head(sink, body, (size_t) (body_end - body));
                break;
            case MKV_SIMPLE_BLOCK:
            case MKV_BLOCK:
                if (*is_opus) webm_block(sink, body, body_end);
                break;
            default:
                break;
        }
        p = body_end;
    }
}

static int webm_decode(const uint8_t *data, const size_t n, VoicePcm *out) {
    OpusSink sink = {0};
    int is_opus = 0;
    webm_walk(&sink, data, data + n, &is_opus);
    return opus_sink_finish(&sink, out);
}

// Ogg Opus (Firefox's MediaRecorder): the first logical stream's packets.
static int ogg_decode(const uint8_t *data, const size_t n, VoicePcm *out) {
    OpusSink sink = {0};
    VoiceBuf packet = {0};
    uint32_t serial = 0;
    int have_serial = 0, index = 0;
    size_t pos = 0;
    while (pos + 27 <= n && !memcmp(data + pos, "OggS", 4) && !sink.failed) {
        const uint8_t *page = data + pos;
        const uint32_t page_serial = (uint32_t) page[14] | (uint32_t) page[15] << 8 | (uint32_t) page[16] << 16 | (uint32_t) page[17] << 24;
        const int segments = page[26];
        if (pos + 27 + (size_t) segments > n) break;
        size_t body = pos + 27 + (size_t) segments;
        if (!have_serial) {
            serial = page_serial;
            have_serial = 1;
        }
        for (int s = 0; s < segments; s++) {
            const size_t lace = page[27 + s];
            if (body + lace > n) break;
            if (page_serial == serial) vbuf_put(&packet, data + body, lace);
            body += lace;
            if (lace < 255 && page_serial == serial) {
                if (index == 0) opus_sink_head(&sink, packet.data, packet.len);
                else if (index >= 2) opus_sink_packet(&sink, packet.data, packet.len); // 1: OpusTags
                index++;
                packet.len = 0;
            }
        }
        pos = body;
    }
    ebml_free(&packet);
    if (index < 1 || !sink.channels) sink.failed = 1;
    return opus_sink_finish(&sink, out);
}

// Howler's load: whatever the data URL holds, decoded to f32.
static int clip_decode(const char *url, VoicePcm *out) {
    size_t n = 0;
    uint8_t *data = data_url_decode(url, &n);
    if (!data || !n) {
        free(data);
        return 0;
    }
    int ok = 0;
    memset(out, 0, sizeof(*out));
    if (n >= 4 && !memcmp(data, "\x1A\x45\xDF\xA3", 4)) {
        ok = opus_available() && webm_decode(data, n, out);
    } else if (n >= 4 && !memcmp(data, "OggS", 4)) {
        ok = opus_available() && ogg_decode(data, n, out);
    } else {
        ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
        ma_uint64 frames = 0;
        void *pcm = NULL;
        if (ma_decode_memory(data, n, &config, &frames, &pcm) == MA_SUCCESS && pcm && frames) {
            out->pcm = malloc((size_t) frames * config.channels * sizeof(float));
            if (out->pcm) {
                memcpy(out->pcm, pcm, (size_t) frames * config.channels * sizeof(float));
                out->frames = frames;
                out->channels = config.channels;
                out->rate = config.sampleRate;
                ok = 1;
            }
        }
        if (pcm) ma_free(pcm, NULL);
    }
    free(data);
    return ok && out->frames && out->channels && out->rate;
}

// ---------------------------------------------------------------- clip encoding

static void wav_header(VoiceBuf *b, const uint32_t rate, const uint32_t samples) {
    vbuf_put(b, "RIFF", 4);
    vbuf_le(b, 36 + samples * 2, 4);
    vbuf_put(b, "WAVEfmt ", 8);
    vbuf_le(b, 16, 4);
    vbuf_le(b, 1, 2);        // PCM
    vbuf_le(b, 1, 2);        // mono
    vbuf_le(b, rate, 4);
    vbuf_le(b, rate * 2, 4);
    vbuf_le(b, 2, 2);
    vbuf_le(b, 16, 2);
    vbuf_put(b, "data", 4);
    vbuf_le(b, samples * 2, 4);
}

static int16_t to_s16(const float v) {
    const float c = v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v;
    return (int16_t) lrintf(c * 32767.0f);
}

// One recorder session's samples (48 kHz mono) -> data URL. Like Chrome, only whole 60 ms
// frames are encoded: the tail of a session never reaches the file.
static char *clip_encode(const float *pcm, const size_t samples, size_t *out_len, int *frames_out) {
    const int frames = (int) (samples / VOICE_FRAME_SAMPLES);
    *frames_out = frames;
    VoiceBuf file = {0};

    if (opus_available()) {
        int error = 0;
        OpusEncoder *encoder = g_opus.encoder_create(VOICE_SAMPLE_RATE, 1, OPUS_APPLICATION_AUDIO, &error);
        if (!encoder) return NULL;
        // MediaRecorder({bitsPerSecond: micQuality}): Chrome gives the Opus encoder VOICE_OPUS_BITRATE of
        // v3.1.1's 6000; other qualities scale alike
        const int32_t bitrate = (int32_t) ((long long) VOICE_OPUS_BITRATE * g_vset.mic_quality / VOICE_BITS_PER_SECOND);
        g_opus.encoder_ctl(encoder, OPUS_SET_BITRATE_REQUEST, bitrate);
        uint8_t *storage = malloc((size_t) (frames ? frames : 1) * 1276);
        const uint8_t **packets = malloc(sizeof(*packets) * (size_t) (frames ? frames : 1));
        int *sizes = malloc(sizeof(*sizes) * (size_t) (frames ? frames : 1));
        int count = 0;
        if (storage && packets && sizes) {
            for (int i = 0; i < frames; i++) {
                uint8_t *dst = storage + (size_t) i * 1276;
                const int32_t n = g_opus.encode_float(encoder, pcm + (size_t) i * VOICE_FRAME_SAMPLES, VOICE_FRAME_SAMPLES, dst, 1276);
                if (n <= 0) continue;
                packets[count] = dst;
                sizes[count] = n;
                count++;
            }
            webm_write(&file, packets, sizes, count);
        }
        g_opus.encoder_destroy(encoder);
        free(storage);
        free(packets);
        free(sizes);
        if (file.failed || !file.len) {
            ebml_free(&file);
            return NULL;
        }
        char *url = b64_data_url(VOICE_DATA_URL_PREFIX, file.data, file.len, out_len);
        ebml_free(&file);
        return url;
    }

    // No libopus: 8 kHz 16-bit PCM WAV (box-filtered 6:1 decimation), playable everywhere.
    const uint32_t ratio = VOICE_SAMPLE_RATE / VOICE_FALLBACK_RATE;
    const uint32_t out_samples = (uint32_t) ((size_t) frames * VOICE_FRAME_SAMPLES / ratio);
    wav_header(&file, VOICE_FALLBACK_RATE, out_samples);
    for (uint32_t i = 0; i < out_samples; i++) {
        float sum = 0.0f;
        for (uint32_t k = 0; k < ratio; k++) sum += pcm[i * ratio + k];
        const int16_t s = to_s16(sum / (float) ratio);
        vbuf_le(&file, (uint16_t) s, 2);
    }
    if (file.failed) {
        ebml_free(&file);
        return NULL;
    }
    char *url = b64_data_url("data:audio/wav;base64,", file.data, file.len, out_len);
    ebml_free(&file);
    return url;
}

// ---------------------------------------------------------------- speaker queues

typedef struct {
    char name[VOICE_NAME_MAX]; // the queue it was added to (hk)
    int refs;           // queue slot + playing instances + a pending decode
    int has_src;        // srcObj: the speaker's player was known when the clip arrived
    int ready;          // isReady
    int loading;        // web: decodeAudioData still running
    int web_id;
    float volume;       // Howl volume: master * voice * 2 at creation (v7: * the speaker's micVolume)
    int sid;            // the speaker's uid (srcObj), -1 unknown
    int spatial;        // v7 voiceChatType 1: positioned at the speaker's head
    float ref_distance; // settings voiceDistance (slider * 5)
    VoicePcm pcm;
} VoiceClip;

typedef struct {
    char name[VOICE_NAME_MAX];
    VoiceClip **clips;
    int count, cap;
} VoiceQueue;

typedef struct {
    int in_use;
    VoiceClip *clip;
    char name[VOICE_NAME_MAX];
    ma_audio_buffer_ref buffer;
    ma_sound sound;
} VoiceInstance;

static Client *g_voice_client; // the client of the last voice_update (srcObj positions at play time)

static struct {
    VoiceQueue queues[VOICE_MAX_SPEAKERS];
    int queue_count;
    VoiceInstance instances[VOICE_MAX_INSTANCES];
    VoiceClip *loading[VOICE_MAX_SPEAKERS * 4]; // web decodes in flight
    int loading_count;
    int dump_index;
} g_play;

static void clip_unref(VoiceClip *clip) {
    if (!clip || --clip->refs > 0) return;
    free(clip->pcm.pcm);
    free(clip);
}

static VoiceQueue *queue_find(const char *name) {
    for (int i = 0; i < g_play.queue_count; i++) {
        if (!strcmp(g_play.queues[i].name, name)) return &g_play.queues[i];
    }
    return NULL;
}

// `delete voiceStreamQueues[name]`
static void queue_delete(VoiceQueue *queue) {
    for (int i = 0; i < queue->count; i++) clip_unref(queue->clips[i]);
    free(queue->clips);
    const int index = (int) (queue - g_play.queues);
    g_play.queues[index] = g_play.queues[--g_play.queue_count];
}

static void instance_release(VoiceInstance *instance) {
    if (!instance->in_use) return;
    ma_sound_uninit(&instance->sound);
    ma_audio_buffer_ref_uninit(&instance->buffer);
    clip_unref(instance->clip);
    instance->in_use = 0;
}

static void rem_speaker(const char *name, int force, int last);

// Howl.play(): another instance of the clip (Howler starts a new sound while one plays).
static void clip_play(VoiceClip *clip, const char *name) {
    ma_engine *engine = audio_engine();
    VoiceInstance *instance = NULL;
    for (int i = 0; i < VOICE_MAX_INSTANCES && !instance; i++) {
        if (!g_play.instances[i].in_use) instance = &g_play.instances[i];
    }
    voice_log("play %s %.3fs vol=%.3f", name, clip->pcm.rate ? (double) clip->pcm.frames / clip->pcm.rate : 0.0, clip->volume);
    if (!engine || !instance || !clip->pcm.frames) {
        rem_speaker(name, 0, 0); // nothing can play: it "ends" right away
        return;
    }
    memset(instance, 0, sizeof(*instance));
    if (ma_audio_buffer_ref_init(ma_format_f32, clip->pcm.channels, clip->pcm.pcm, clip->pcm.frames, &instance->buffer) != MA_SUCCESS) {
        rem_speaker(name, 0, 0);
        return;
    }
    instance->buffer.sampleRate = clip->pcm.rate;
    // v7 voiceChatType 1: hm.pos(srcObj head) at play time, pannerAttr refDistance voiceDistance,
    // rolloff 1 (Howler's inverse model; v7 sets the panner only for a queue's first clip, the
    // later ones fall back to Howler's refDistance 1: every clip gets the setting here).
    const Player *src = clip->spatial && g_voice_client && clip->sid >= 0 ? game_find_player(&g_voice_client->game, clip->sid) : NULL;
    const ma_uint32 flags = src ? 0 : MA_SOUND_FLAG_NO_SPATIALIZATION;
    if (ma_sound_init_from_data_source(engine, &instance->buffer, flags, NULL, &instance->sound) != MA_SUCCESS) {
        ma_audio_buffer_ref_uninit(&instance->buffer);
        rem_speaker(name, 0, 0);
        return;
    }
    if (src) {
        ma_sound_set_position(&instance->sound, src->position.x, src->position.y + src->height - game_constants.camera_height, src->position.z);
        ma_sound_set_attenuation_model(&instance->sound, ma_attenuation_model_inverse);
        ma_sound_set_min_distance(&instance->sound, clip->ref_distance);
        ma_sound_set_max_distance(&instance->sound, 10000.0f); // Howler panner maxDistance
        ma_sound_set_rolloff(&instance->sound, 1.0f);
        ma_sound_set_doppler_factor(&instance->sound, 0.0f);
    }
    instance->in_use = 1;
    instance->clip = clip;
    clip->refs++;
    snprintf(instance->name, sizeof(instance->name), "%s", name);
    ma_sound_set_volume(&instance->sound, clip->volume);
    ma_sound_start(&instance->sound);
}

// window.remSpeaker(name, force, last)
static void rem_speaker(const char *name, const int force, const int last) {
    VoiceQueue *queue = queue_find(name);
    if (queue && queue->count) {
        VoiceClip *removed;
        if (last) {
            removed = queue->clips[--queue->count];
        } else {
            removed = queue->clips[0];
            memmove(queue->clips, queue->clips + 1, sizeof(*queue->clips) * (size_t) (queue->count - 1));
            queue->count--;
        }
        clip_unref(removed);
        // v7 f1769: the next ready clip plays whether or not its speaker is a known player (v3.1.1
        // also wanted srcObj, which left clips of spectators and unseen speakers stuck forever)
        if (queue->count && queue->clips[0]->ready) clip_play(queue->clips[0], name);
        queue = queue_find(name); // clip_play may have ended the queue already
    }
    if (queue && (!queue->count || force)) queue_delete(queue);
}

static void dump_clip(const VoicePcm *pcm) {
    const char *dir = getenv("KRUNKNATIVE_VOICE_DUMP");
    if (!dir || !*dir) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/vc-%03d.wav", dir, g_play.dump_index++);
    FILE *file = fopen(path, "wb");
    if (!file) return;
    VoiceBuf b = {0};
    const uint32_t samples = (uint32_t) pcm->frames; // first channel
    wav_header(&b, pcm->rate, samples);
    for (uint32_t i = 0; i < samples; i++) vbuf_le(&b, (uint16_t) to_s16(pcm->pcm[(size_t) i * pcm->channels]), 2);
    if (!b.failed) fwrite(b.data, 1, b.len, file);
    fclose(file);
    ebml_free(&b);
}

// hm.once("load"): ready and played when first in its queue, or dropped when too long.
static void clip_loaded(VoiceClip *clip) {
    const char *name = clip->name;
    clip->refs++; // remSpeaker below may drop its queue slot
    const double duration = clip->pcm.rate ? (double) clip->pcm.frames / clip->pcm.rate : 0.0;
    voice_log("load %s %.3fs (%u Hz, %u ch)", name, duration, clip->pcm.rate, clip->pcm.channels);
    dump_clip(&clip->pcm);
    if (duration <= VOICE_CHAT_MAX_LENGTH * 1.2) {
        clip->ready = 1;
        VoiceQueue *queue = queue_find(name);
        // v7 queueVoiceChatAudio onload: plays when first in its queue, srcObj or not (only the
        // spatial placement needs it)
        if (queue && queue->count && queue->clips[0] == clip) clip_play(clip, name);
    } else {
        rem_speaker(name, 0, 1);
    }
    clip_unref(clip);
}

#ifdef __EMSCRIPTEN__
// Howler on the web: fetch the data URL and decodeAudioData it (48 kHz, like the AudioContext
// miniaudio runs). Results are polled from voice_update(), never called back into C.
EM_JS(int, voice_web_decode_start, (const char *url), {
    var v = window.knVoice = window.knVoice || {clips: [], decodes: {}, nextId: 1, state: 0};
    var id = v.nextId++;
    var entry = v.decodes[id] = {state: 0, data: null, channels: 0, frames: 0};
    var text = UTF8ToString(url);
    try {
        fetch(text).then(function (r) { return r.arrayBuffer(); }).then(function (buf) {
            var Ctx = window.OfflineAudioContext || window.webkitOfflineAudioContext;
            var ctx = new Ctx(1, 1, 48000);
            return new Promise(function (ok, fail) { ctx.decodeAudioData(buf, ok, fail); });
        }).then(function (audio) {
            var ch = audio.numberOfChannels, n = audio.length, out = new Float32Array(n * ch);
            for (var c = 0; c < ch; c++) {
                var src = audio.getChannelData(c);
                for (var i = 0; i < n; i++) out[i * ch + c] = src[i];
            }
            entry.data = out; entry.channels = ch; entry.frames = n; entry.rate = audio.sampleRate; entry.state = 1;
        }).catch(function () { entry.state = -1; });
    } catch (e) { entry.state = -1; }
    return id;
});

// 0 pending, -1 failed (Howler: loaderror, nothing listens), 1 decoded.
EM_JS(int, voice_web_decode_state, (int id, int *frames, int *channels, int *rate), {
    var v = window.knVoice, e = v && v.decodes[id];
    if (!e) return -1;
    if (e.state === 1) { HEAP32[frames >> 2] = e.frames; HEAP32[channels >> 2] = e.channels; HEAP32[rate >> 2] = e.rate; }
    if (e.state === -1) delete v.decodes[id];
    return e.state;
});

EM_JS(void, voice_web_decode_take, (int id, float *dst), {
    var v = window.knVoice, e = v && v.decodes[id];
    if (!e) return;
    if (e.data) HEAPF32.set(e.data, dst >> 2);
    delete v.decodes[id];
});
#endif

static void clip_failed(VoiceClip *clip, size_t bytes);
static void voice_notice(const char *fmt, ...);

static void voice_poll_loading(void) {
#ifdef __EMSCRIPTEN__
    for (int i = 0; i < g_play.loading_count;) {
        VoiceClip *clip = g_play.loading[i];
        int frames = 0, channels = 0, rate = 0;
        const int state = voice_web_decode_state(clip->web_id, &frames, &channels, &rate);
        if (state == 0) {
            i++;
            continue;
        }
        g_play.loading[i] = g_play.loading[--g_play.loading_count];
        clip->loading = 0;
        if (state > 0 && frames > 0 && channels > 0) {
            clip->pcm.pcm = malloc((size_t) frames * (size_t) channels * sizeof(float));
            if (clip->pcm.pcm) {
                voice_web_decode_take(clip->web_id, clip->pcm.pcm);
                clip->pcm.frames = (uint64_t) frames;
                clip->pcm.channels = (uint32_t) channels;
                clip->pcm.rate = (uint32_t) rate;
            }
        }
        // "load" fires whether or not the clip is still queued (initGame may have reset the
        // queues meanwhile); a failed decode never fires it.
        if (clip->pcm.pcm) clip_loaded(clip);
        else clip_failed(clip, 0);
        clip_unref(clip); // the decode's reference
    }
#endif
}

// KrunkNative fix: a clip that cannot be decoded leaves its queue. Howler never fires "load" for
// one, so in v3.1.1 / v7 it stays first in voiceStreamQueues[name] for good and that speaker is
// never heard again (an Opus clip on a client without Opus, a Safari MP4 clip, a cut-off file).
static void clip_failed(VoiceClip *clip, const size_t bytes) {
    static int warned;
    voice_log("cannot decode clip from %s (%zu bytes)", clip->name, bytes);
    if (!warned) {
        warned = 1;
        fprintf(stderr, "[voice] a voice clip from %s could not be decoded (%zu bytes); it is skipped\n", clip->name, bytes);
    }
    VoiceQueue *queue = queue_find(clip->name);
    if (!queue) return;
    for (int i = 0; i < queue->count; i++) {
        if (queue->clips[i] != clip) continue;
        memmove(queue->clips + i, queue->clips + i + 1, sizeof(*queue->clips) * (size_t) (queue->count - i - 1));
        queue->count--;
        const int was_first = i == 0;
        char name[VOICE_NAME_MAX];
        snprintf(name, sizeof(name), "%s", clip->name);
        clip_unref(clip); // the queue slot
        if (was_first && queue->count && queue->clips[0]->ready) clip_play(queue->clips[0], name);
        queue = queue_find(name);
        if (queue && !queue->count) queue_delete(queue);
        return;
    }
}

static void clip_start_decode(VoiceClip *clip, const char *audio) {
    // Opus (WebM / Ogg) and WAV / MP3 decode in C on every platform (the web build links libopus
    // too: Safari's decodeAudioData has no WebM Opus). Anything else goes to the browser.
    if (clip_decode(audio, &clip->pcm)) {
        clip_loaded(clip);
        return;
    }
#ifdef __EMSCRIPTEN__
    if (g_play.loading_count < (int) (sizeof(g_play.loading) / sizeof(g_play.loading[0]))) {
        clip->loading = 1;
        clip->web_id = voice_web_decode_start(audio);
        clip->refs++;
        g_play.loading[g_play.loading_count++] = clip;
        return;
    }
#endif
    clip_failed(clip, strlen(audio));
}

// window.addVoiceStream(player, name, audio) / v7 queueVoiceChatAudio: `volume` multiplies the Howl
// volume (v7: the speaker's micVolume), `sid` is srcObj (for proximity voice).
static void add_voice_stream(const int has_src, const char *name, const char *audio, const float volume, const int sid) {
    VoiceQueue *queue = queue_find(name);
    if (!queue) {
        if (g_play.queue_count >= VOICE_MAX_SPEAKERS) return;
        queue = &g_play.queues[g_play.queue_count++];
        memset(queue, 0, sizeof(*queue));
        snprintf(queue->name, sizeof(queue->name), "%s", name);
    }
    if (queue->count >= VOICE_MAX_QUEUE) return;
    if (queue->count == queue->cap) {
        const int cap = queue->cap ? queue->cap * 2 : 8;
        VoiceClip **grown = realloc(queue->clips, sizeof(*grown) * (size_t) cap);
        if (!grown) return;
        queue->clips = grown;
        queue->cap = cap;
    }
    VoiceClip *clip = calloc(1, sizeof(*clip));
    if (!clip) return;
    clip->refs = 1;
    snprintf(clip->name, sizeof(clip->name), "%s", name);
    clip->has_src = has_src;
    clip->volume = audio_get_volume(AUDIO_CAT_MASTER) * audio_get_volume(AUDIO_CAT_VOICE) * volume * 2.0f;
    clip->sid = sid;
    clip->spatial = has_src && g_vset.chat_type == 1; // !!sourcePlayer && !!voiceChatType
    clip->ref_distance = g_vset.distance;
    queue->clips[queue->count++] = clip;
    clip->refs++; // held across the decode (clip_loaded / clip_failed may drop the queue slot)
    clip_start_decode(clip, audio);
    clip_unref(clip);
}

// v7 queueVoiceChatAudio's drops: muteSpec (we have no sid: spectating, or the speaker is not
// active) and the speaker's account in mutedVoiceAccountIds.
static int voice_dropped(const Client *client, const Player *speaker, const int account_id) {
    // v7's spectator is an inactive entry of Game.players (sourcePlayer && !sourcePlayer.active);
    // KrunkNative sends spectators no SPAWN, so a speaker without a player is one too.
    if (g_vset.mute_spec && (!client || !client->me || spectate_active() || !speaker || !speaker->active)) return 1;
    return social_voice_muted(account_id);
}

// window.voiceChat(audio, name, sid) (older servers: every channel, full volume)
// voiceVolume 0 (the Pro preset's value) drops every clip we receive, as in v7; say so once.
static int voice_volume_off(const char *name) {
    if (audio_get_volume(AUDIO_CAT_VOICE) > 0.0f) return 0;
    static int warned;
    if (!warned) {
        warned = 1;
        voice_notice("Voice: %s is talking, but your Voice Chat volume is 0 (Settings > Audio)", name);
    }
    return 1;
}

// The speaker's name as the chat shows it (Hide player names, by its sid's uid; the local player is "Player")
static const char *voice_shown_name(const Client *client, const int sid, const char *name, char *out, const size_t size) {
    const int self = sid >= 0 && client && client->me && sid == client->me->uid;
    const char *raw = name && name[0] ? name : "unknown";
    const char *shown = player_names_shown(sid, self, raw, out, size);
    return shown ? shown : raw;
}

static void voice_chat(Client *client, const char *audio, const char *name, const int sid) {
    char shown[PLAYER_NAME_SIZE];
    if (voice_volume_off(voice_shown_name(client, sid, name, shown, sizeof(shown)))) return; // if (SOUND.volumes[1])
    char scrambled[VOICE_NAME_MAX];
    name = streamer_name(name, scrambled, sizeof(scrambled)); // streamerModeEnabled && Filter: UTILS.scrambleS(hk)
    const Player *player = client && sid >= 0 ? game_find_player(&client->game, sid) : NULL;
    const NetPlayerInfoPacket *info = sid >= 0 ? player_accounts_get(sid) : NULL;
    if (voice_dropped(client, player, info ? info->account_id : 0)) return;
    voice_log("recv %s sid=%d %zu bytes%s", name, sid, strlen(audio), player ? "" : " (unknown player)");
    add_voice_stream(player != NULL, name, audio, 1.0f, sid);
}

// v7 io_voiceChat(audio, name, sid, channel, volume, isFinal, accountId): SOUND.volumes[1] * volume
// must be > 0 and the channel -1 or ours (vLN02023). isFinal (never set by v7's server) is ignored.
static void voice_chat_v7(Client *client, const char *audio, const char *name, const int sid, const VoiceV7Meta *meta) {
    char shown[PLAYER_NAME_SIZE];
    if (voice_volume_off(voice_shown_name(client, sid, name, shown, sizeof(shown))) || !(meta->volume > 0.0f)) return; // SOUND.volumes[1] * volume > 0
    if (meta->channel != CHAT_CHANNEL_ALL && meta->channel != hud_ui_chat_channel()) return;
    char scrambled[VOICE_NAME_MAX];
    name = streamer_name(name, scrambled, sizeof(scrambled));
    const Player *player = client && sid >= 0 ? game_find_player(&client->game, sid) : NULL;
    if (voice_dropped(client, player, meta->account_id)) return;
    voice_log("recv v7 %s sid=%d ch=%d vol=%.2f %zu bytes%s", name, sid, meta->channel, (double) meta->volume, strlen(audio), player ? "" : " (unknown player)");
    add_voice_stream(player != NULL, name, audio, meta->volume, sid);
}

static void instances_update(void) {
    for (int i = 0; i < VOICE_MAX_INSTANCES; i++) {
        VoiceInstance *instance = &g_play.instances[i];
        if (!instance->in_use || !ma_sound_at_end(&instance->sound)) continue;
        char name[VOICE_NAME_MAX];
        snprintf(name, sizeof(name), "%s", instance->name);
        instance_release(instance);
        voice_log("end %s", name);
        rem_speaker(name, 0, 0); // hm.on("end")
    }
}

void voice_reset_queues(void) {
    while (g_play.queue_count) queue_delete(&g_play.queues[g_play.queue_count - 1]);
}

int voice_is_speaking(const char *name) {
    const VoiceQueue *queue = name ? queue_find(name) : NULL;
    return queue && queue->count > 0;
}

const char *voice_speaker_icon(void) {
    return (long long) (glfwGetTime() * 1000.0 / VOICE_ICON_MS) % 2 ? "volume_up" : "volume_down";
}

void voice_audio_fini(void) {
    for (int i = 0; i < VOICE_MAX_INSTANCES; i++) instance_release(&g_play.instances[i]);
}

// ---------------------------------------------------------------- receive

static SocialAssembly g_receive;
static SocialAssembly g_receive_v7;

void voice_on_packet(Client *client, const void *payload, const uint16_t length) {
    char *msg = NULL;
    uint32_t sid = 0;
    if (social_assembly_push(&g_receive, payload, length, &msg, &sid) != 1) return;
    char alias[VOICE_NAME_MAX];
    const char *audio = NULL;
    if (voice_parse_relay(msg, alias, sizeof(alias), &audio)) voice_chat(client, audio, alias, (int) (int32_t) sid);
    free(msg);
}

void voice_on_packet_v7(Client *client, const void *payload, const uint16_t length) {
    char *msg = NULL;
    uint32_t sid = 0;
    if (social_assembly_push(&g_receive_v7, payload, length, &msg, &sid) != 1) return;
    char alias[VOICE_NAME_MAX];
    const char *audio = NULL;
    VoiceV7Meta meta;
    if (voice_parse_relay_v7(msg, &meta, alias, sizeof(alias), &audio)) voice_chat_v7(client, audio, alias, (int) (int32_t) sid, &meta);
    free(msg);
}

void voice_apply_setting(const char *key, const char *value) {
    if (!key || !value) return;
    if (!strcmp(key, "micQuality")) {
        const int q = atoi(value); // parseInt(SETTINGS.tmp.micQuality)
        g_vset.mic_quality = q > 0 ? q : VOICE_MIC_QUALITY_DEFAULT;
    } else if (!strcmp(key, "micVolume")) {
        const float v = (float) atof(value);
        g_vset.mic_volume = v == v ? v : 1.0f;
    } else if (!strcmp(key, "voiceChatType")) {
        g_vset.chat_type = atoi(value) ? 1 : 0;
        // settings.list.voiceDistance.hide = value == 0: #voiceDistance_div follows
        const HtmlElem row = html_dom_by_id("voiceDistance_div");
        if (row) html_dom_set_style(row, "display", g_vset.chat_type ? "block" : "none");
    } else if (!strcmp(key, "voiceDistance")) {
        const int d = atoi(value);
        g_vset.distance = (float) (d ? d : 1) * 5.0f; // (parseInt(v) || 1) * 5
    } else if (!strcmp(key, "muteSpec")) {
        g_vset.mute_spec = !strcmp(value, "true") || !strcmp(value, "1");
    } else if (!strcmp(key, "voiceChatTrigger")) {
        g_vset.trigger = atoi(value) == 0 && value[0] == '0' ? 0 : 1;
    }
}

// ---------------------------------------------------------------- capture

static struct {
    int recording;       // cB
    int ready;           // cC
    int session;         // a getUserMedia stream + the voiceRate interval (cA)
    int pending;         // web: getUserMedia not answered yet
    int recorder_on;     // recorder.state == "recording"
    double next_tick;
    float *pcm;          // this recorder run's samples (48 kHz mono)
    size_t samples, cap;
    uint32_t seq;
    char input[256];     // settings.tmp.audioInput
} g_rec = {.ready = 1};

// ---- status: what the player sees of the microphone (KrunkNative addition, v7 shows nothing:
// a refused getUserMedia only leaves the icon on mic_off). Failures go to the chat as a system
// line ("Voice unavailable: <reason>") and the console; #voiceIcon shows the live input level.

static void voice_display_opacity(const char *opacity);

#define VOICE_LEVEL_SILENT 1e-4f   // RMS below this for VOICE_SILENT_SECONDS: a muted / denied mic
#define VOICE_SILENT_SECONDS 1.5
#define VOICE_NO_DATA_SECONDS 1.0  // a session whose device delivered nothing at all after this

static struct {
    float level;            // RMS of the last drained block, 0..1
    double session_start;   // glfwGetTime() of session_opened
    double loud_at;         // last time the level was over VOICE_LEVEL_SILENT
    uint64_t session_samples;
    int warned_silent;      // once per session
    int warned_mic_volume;  // once per run: micVolume 0 (the Pro preset) makes every clip silent
    char last_error[160];
} g_status;

static void voice_notice(const char *fmt, ...) {
    char text[200];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    fprintf(stderr, "[voice] %s\n", text);
#ifdef UI_SCENARIO_SUPPORTED
    if (ui_scenario_active()) return;
#endif
    hud_ui_add_chat(NULL, text, "#fc03ec"); // a server-coloured system line
}

static void voice_unavailable(const char *reason) {
    snprintf(g_status.last_error, sizeof(g_status.last_error), "%s", reason);
    voice_notice("Voice unavailable: %s", reason);
}

const char *voice_last_error(void) {
    return g_status.last_error;
}

float voice_input_level(void) {
    return g_status.level;
}

// #voiceIcon while recording: the mic glyph's brightness follows the input level, so a dead
// microphone is visible at once (the icon stays dim).
static void voice_show_level(void) {
    const HtmlElem icon = html_dom_by_id("voiceIcon");
    if (!icon) return;
    char opacity[16];
    const float lit = g_status.level > 0.0f ? 0.45f + 0.55f * fminf(1.0f, sqrtf(g_status.level * 8.0f)) : 0.45f;
    snprintf(opacity, sizeof(opacity), "%.2f", (double) lit);
    html_dom_set_style(icon, "opacity", opacity);
}

// One drained block of `n` mono samples.
static void voice_note_samples(const float *pcm, const size_t n) {
    if (!n) return;
    double sum = 0.0;
    for (size_t i = 0; i < n; i++) sum += (double) pcm[i] * pcm[i];
    g_status.level = (float) sqrt(sum / (double) n);
    g_status.session_samples += n;
    if (g_status.level > VOICE_LEVEL_SILENT) g_status.loud_at = glfwGetTime();
}

// Per frame while a session runs: a device that delivers nothing, or only digital silence (macOS
// without microphone permission, a hardware mute switch, the wrong input picked).
static void voice_check_session(void) {
    const double now = glfwGetTime();
    if (g_status.warned_silent) return;
    if (!g_status.session_samples && now - g_status.session_start > VOICE_NO_DATA_SECONDS) {
        g_status.warned_silent = 1;
        voice_unavailable("the microphone delivers no audio (check the input device and its permission)");
    } else if (g_status.session_samples && now - g_status.loud_at > VOICE_SILENT_SECONDS &&
               now - g_status.session_start > VOICE_SILENT_SECONDS) {
        g_status.warned_silent = 1;
        voice_notice("Voice: the microphone is silent (muted, or no permission to use it)");
    }
}

// Captured samples (48 kHz mono) -> the level meter and, while the recorder runs, this run's clip.
static void rec_append(const float *pcm, const size_t n) {
    voice_note_samples(pcm, n);
    if (!g_rec.recorder_on || !n) return;
    if (g_rec.samples + n > g_rec.cap) {
        size_t cap = g_rec.cap ? g_rec.cap : VOICE_SAMPLE_RATE;
        while (cap < g_rec.samples + n) cap *= 2;
        float *grown = realloc(g_rec.pcm, cap * sizeof(float));
        if (!grown) return;
        g_rec.pcm = grown;
        g_rec.cap = cap;
    }
    memcpy(g_rec.pcm + g_rec.samples, pcm, n * sizeof(float));
    g_rec.samples += n;
}

#ifndef __EMSCRIPTEN__
// KRUNKNATIVE_VOICE_INPUT=tone:<hz> | file:<wav> (tests): a generated sine at 0.3 amplitude or a
// WAV file (looped, any rate / channel count, mixed down and resampled to 48 kHz) instead of a
// capture device, fed in real time (glfwGetTime) like a microphone. No audio device is opened.
static struct {
    int on;
    double hz;
    float *wav;          // 48 kHz mono
    size_t wav_frames, wav_pos;
    double phase;
    double fed_until;    // glfwGetTime() the samples so far cover
} g_synth;

static struct {
    int context_ready;
    ma_context context;
    int device_open;
    ma_device device;
    int rb_ready;
    ma_pcm_rb rb;
    char opened_input[256]; // the input the open device was asked for
} g_cap;

// Reader side of the ring: drop everything captured so far (safe while the device runs).
static void capture_discard(void) {
    if (!g_cap.rb_ready) return;
    for (ma_uint32 n; (n = ma_pcm_rb_available_read(&g_cap.rb)) != 0;) {
        void *src = NULL;
        if (ma_pcm_rb_acquire_read(&g_cap.rb, &n, &src) != MA_SUCCESS || !n) return;
        ma_pcm_rb_commit_read(&g_cap.rb, n);
    }
}

static void capture_callback(ma_device *device, void *output, const void *input, const ma_uint32 frames) {
    (void) device;
    (void) output;
    ma_uint32 done = 0;
    while (done < frames) {
        ma_uint32 n = frames - done;
        void *dst = NULL;
        if (ma_pcm_rb_acquire_write(&g_cap.rb, &n, &dst) != MA_SUCCESS || !n) return; // full: dropped
        memcpy(dst, (const float *) input + done, n * sizeof(float));
        ma_pcm_rb_commit_write(&g_cap.rb, n);
        done += n;
    }
}

// Same backends as playback (audio_backends: KRUNKNATIVE_AUDIO_BACKEND, else every backend
// of the platform in miniaudio's order, e.g. PulseAudio/PipeWire, ALSA, JACK on Linux).
static int capture_context(void) {
    if (g_cap.context_ready) return 1;
    int list[16];
    ma_backend backends[16];
    const int count = audio_backends(list, 16);
    for (int i = 0; i < count; i++) backends[i] = (ma_backend) list[i];
    if (ma_context_init(count ? backends : NULL, (ma_uint32) count, NULL, &g_cap.context) != MA_SUCCESS) return 0;
    g_cap.context_ready = 1;
    voice_log("capture backend %s", ma_get_backend_name(g_cap.context.backend));
    return 1;
}

static void capture_close(void) {
    if (g_cap.device_open) ma_device_uninit(&g_cap.device);
    g_cap.device_open = 0;
    g_synth.on = 0;
}

static int synth_open(const char *spec) {
    free(g_synth.wav);
    memset(&g_synth, 0, sizeof(g_synth));
    if (!strncmp(spec, "tone:", 5)) {
        g_synth.hz = atof(spec + 5);
        if (!(g_synth.hz > 0.0 && g_synth.hz < VOICE_SAMPLE_RATE / 2)) return 0;
    } else {
        ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 1, VOICE_SAMPLE_RATE);
        ma_uint64 frames = 0;
        void *pcm = NULL;
        if (ma_decode_file(spec + 5, &config, &frames, &pcm) != MA_SUCCESS || !frames) return 0;
        g_synth.wav = malloc((size_t) frames * sizeof(float));
        if (g_synth.wav) memcpy(g_synth.wav, pcm, (size_t) frames * sizeof(float));
        ma_free(pcm, NULL);
        if (!g_synth.wav) return 0;
        g_synth.wav_frames = (size_t) frames;
    }
    g_synth.on = 1;
    g_synth.fed_until = glfwGetTime();
    return 1;
}

// Writes the samples the synthetic microphone produced since the last call into the ring.
static void synth_feed(void) {
    if (!g_synth.on || !g_cap.rb_ready) return;
    const double now = glfwGetTime();
    size_t want = (size_t) ((now - g_synth.fed_until) * VOICE_SAMPLE_RATE);
    if (!want) return;
    g_synth.fed_until += (double) want / VOICE_SAMPLE_RATE;
    while (want) {
        ma_uint32 n = (ma_uint32) (want < 4096 ? want : 4096);
        void *dst = NULL;
        if (ma_pcm_rb_acquire_write(&g_cap.rb, &n, &dst) != MA_SUCCESS || !n) return; // full: dropped, like the device
        float *out = dst;
        for (ma_uint32 i = 0; i < n; i++) {
            if (g_synth.wav) {
                out[i] = g_synth.wav[g_synth.wav_pos];
                g_synth.wav_pos = (g_synth.wav_pos + 1) % g_synth.wav_frames;
            } else {
                out[i] = (float) (0.3 * sin(g_synth.phase));
                g_synth.phase += 2.0 * M_PI * g_synth.hz / VOICE_SAMPLE_RATE;
                if (g_synth.phase > 2.0 * M_PI) g_synth.phase -= 2.0 * M_PI;
            }
        }
        ma_pcm_rb_commit_write(&g_cap.rb, n);
        want -= n;
    }
}

// getUserMedia({audio: audioInput != "default" ? {deviceId: {exact}} : true})
// The device stays open after a session, like v3.1.1's never-stopped getUserMedia stream;
// the next session reuses it unless the requested input changed (v3.1.1 asks getUserMedia
// again with the current audioInput every session).
static int capture_open(void) {
    const char *forced = getenv("KRUNKNATIVE_VOICE_INPUT"); // tests: a virtual microphone by name, or tone: / file:
    const char *input = forced && *forced ? forced : g_rec.input;
    const int synthetic = !strncmp(input, "tone:", 5) || !strncmp(input, "file:", 5);
    if ((g_cap.device_open || g_synth.on) && !strcmp(g_cap.opened_input, input)) {
        // Between sessions nobody reads the ring (the callback drops what does not fit):
        // whatever is in it is stale, the session records from now on.
        capture_discard();
        if (g_synth.on) g_synth.fed_until = glfwGetTime();
        return 1;
    }
    capture_close();
    if (!g_cap.rb_ready) {
        if (ma_pcm_rb_init(ma_format_f32, 1, VOICE_CAPTURE_FRAMES, NULL, NULL, &g_cap.rb) != MA_SUCCESS) {
            voice_unavailable("out of memory");
            return 0;
        }
        g_cap.rb_ready = 1;
    }
    capture_discard(); // the audio thread is stopped here: nothing races the reader

    if (synthetic) {
        if (!synth_open(input)) {
            voice_unavailable("the test input could not be opened");
            return 0;
        }
        snprintf(g_cap.opened_input, sizeof(g_cap.opened_input), "%s", input);
        voice_log("microphone open: synthetic %s", input);
        return 1;
    }

    if (!capture_context()) {
        voice_unavailable("no audio backend for the microphone");
        return 0;
    }
    ma_device_config config = ma_device_config_init(ma_device_type_capture);
    config.capture.format = ma_format_f32;
    config.capture.channels = 1;         // miniaudio mixes a stereo / multichannel mic down
    config.sampleRate = VOICE_SAMPLE_RATE; // and resamples 44.1 kHz etc. to Opus' 48 kHz
    config.dataCallback = capture_callback;

    ma_device_id id;
    if (input[0] && strcmp(input, "default") != 0) {
        ma_device_info *infos = NULL;
        ma_uint32 count = 0;
        int found = 0;
        if (ma_context_get_devices(&g_cap.context, NULL, NULL, &infos, &count) == MA_SUCCESS) {
            for (ma_uint32 i = 0; i < count && !found; i++) {
                if (!strcmp(infos[i].name, input)) {
                    id = infos[i].id;
                    found = 1;
                }
            }
        }
        // KrunkNative difference: v7's {exact} deviceId fails for good once the saved device is
        // unplugged or renamed (OverconstrainedError: voice never works again until the setting is
        // changed). Fall back to the default microphone and say so.
        if (found) config.capture.pDeviceID = &id;
        else if (!forced) voice_notice("Voice: input \"%s\" not found, using the default microphone", input);
        else {
            voice_unavailable("the requested input device is not present");
            return 0;
        }
    }

    const ma_result r = ma_device_init(&g_cap.context, &config, &g_cap.device);
    if (r != MA_SUCCESS) {
        char reason[160];
        snprintf(reason, sizeof(reason), "cannot open the microphone (%s: %s)", ma_get_backend_name(g_cap.context.backend), ma_result_description(r));
        voice_unavailable(reason);
        return 0;
    }
    g_cap.device_open = 1;
    snprintf(g_cap.opened_input, sizeof(g_cap.opened_input), "%s", input);
    if (ma_device_start(&g_cap.device) != MA_SUCCESS) {
        capture_close();
        voice_unavailable("the microphone would not start");
        return 0;
    }
    voice_log("microphone open: %s (%s)", g_cap.device.capture.name, ma_get_backend_name(g_cap.context.backend));
    return 1;
}

static void capture_drain(void) {
    if (!g_cap.rb_ready) return;
    synth_feed();
    for (;;) {
        ma_uint32 n = ma_pcm_rb_available_read(&g_cap.rb);
        if (!n) return;
        void *src = NULL;
        if (ma_pcm_rb_acquire_read(&g_cap.rb, &n, &src) != MA_SUCCESS || !n) return;
        rec_append(src, n);
        ma_pcm_rb_commit_read(&g_cap.rb, n);
    }
}
#else
// Web capture (KrunkNative change): getUserMedia -> a ScriptProcessor tap on the stream at the
// page's AudioContext rate, mixed down to mono by the node (channelCount 1, explicit); C pulls the
// samples (voice_web_take_pcm), resamples them to 48 kHz and encodes the clips with the same
// libopus + WebM writer as native. v3.1.1 / v7 used MediaRecorder, whose files differ per browser
// (Chrome WebM, Firefox Ogg, Safari MP4/AAC that no other client can decode). Firefox refuses to
// connect a stream to a context of another sample rate, hence no fixed 48 kHz context.
// No UI logic here: C owns the session, the interval, #voiceIcon and the messages.
EM_JS(void, voice_web_open, (const char *device), {
    var v = window.knVoice = window.knVoice || {decodes: {}, nextId: 1, state: 0};
    var id = UTF8ToString(device);
    v.state = 1;
    v.error = "";
    var fail = function (e) {
        var n = e && e.name || "";
        v.error = n == "NotAllowedError" || n == "SecurityError" ? "microphone permission denied (allow it in the browser's site settings)"
            : n == "NotFoundError" ? "no microphone found"
            : n == "NotReadableError" ? "the microphone is in use by another program or blocked by the system"
            : "the microphone could not be opened (" + (n || String(e)) + ")";
        v.state = 3;
    };
    if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
        v.error = window.isSecureContext === false ? "the page is not served over https, so the browser allows no microphone"
            : "this browser has no microphone access (getUserMedia)";
        v.state = 3;
        return;
    }
    var open = function (constraints, retry) {
        navigator.mediaDevices.getUserMedia({audio: constraints, video: false}).then(function (stream) {
            if (window.knVoiceInputs) window.knVoiceInputs();
            if (v.stream && v.stream !== stream) v.stream.getTracks().forEach(function (t) { t.stop(); });
            v.stream = stream;
            var Ctx = window.AudioContext || window.webkitAudioContext;
            if (!v.ctx) v.ctx = new Ctx();
            if (v.ctx.state == "suspended" && v.ctx.resume) v.ctx.resume();
            if (v.source) v.source.disconnect();
            if (!v.node) {
                v.node = v.ctx.createScriptProcessor(4096, 1, 1);
                v.node.channelCount = 1;
                v.node.channelCountMode = "explicit";
                v.node.channelInterpretation = "speakers";
                v.chunks = [];
                v.queued = 0;
                v.node.onaudioprocess = function (e) {
                    if (!v.capturing || v.queued > v.ctx.sampleRate * 2) return; // 2 s backlog: drop, like the native ring
                    var data = new Float32Array(e.inputBuffer.getChannelData(0));
                    v.chunks.push(data);
                    v.queued += data.length;
                };
                var mute = v.ctx.createGain();
                mute.gain.value = 0;
                v.node.connect(mute);
                mute.connect(v.ctx.destination); // a ScriptProcessor only runs when it reaches the destination
            }
            v.source = v.ctx.createMediaStreamSource(stream);
            v.source.connect(v.node);
            v.state = 2;
        }).catch(function (e) {
            // KrunkNative difference: v7 asks for {exact: audioInput} and fails for good once that
            // device is gone; fall back to the default microphone
            if (retry && e && e.name == "OverconstrainedError") { v.fallback = 1; open(true, false); }
            else fail(e);
        });
    };
    try { open(id && id != "default" ? {deviceId: {exact: id}} : true, !!(id && id != "default")); } catch (e) { fail(e); }
});

// 0 idle, 1 asking, 2 open, 3 refused (voice_web_error says why)
EM_JS(int, voice_web_state, (void), {
    return window.knVoice ? window.knVoice.state : 0;
});

EM_JS(char *, voice_web_error, (void), {
    var v = window.knVoice;
    var text = v && v.fallback ? (v.fallback = 0, "\x01") : v && v.error ? v.error : "";
    return stringToNewUTF8(text);
});

EM_JS(int, voice_web_rate, (void), {
    return window.knVoice && window.knVoice.ctx ? window.knVoice.ctx.sampleRate | 0 : 0;
});

EM_JS(void, voice_web_capture, (int on), {
    var v = window.knVoice;
    if (!v) return;
    v.capturing = on;
    v.chunks = [];
    v.queued = 0;
});

// Up to `max` captured samples into dst; returns how many.
EM_JS(int, voice_web_take_pcm, (float *dst, int max), {
    var v = window.knVoice;
    if (!v || !v.chunks || !v.chunks.length) return 0;
    var n = 0;
    while (v.chunks.length && n < max) {
        var c = v.chunks[0];
        var take = Math.min(c.length, max - n);
        HEAPF32.set(take == c.length ? c : c.subarray(0, take), (dst >> 2) + n);
        n += take;
        if (take == c.length) v.chunks.shift(); else v.chunks[0] = c.subarray(take);
    }
    v.queued -= n;
    return n;
});

// window.updateAudioInputs / getAudioInputs
EM_JS(char *, voice_web_inputs, (void), {
    if (!window.knVoiceInputs) {
        window.knVoiceInputOptions = {"default": "Default"};
        window.knVoiceInputs = function () {
            if (navigator && navigator.mediaDevices && navigator.mediaDevices.enumerateDevices) {
                navigator.mediaDevices.enumerateDevices().then(function (devices) {
                    var inputs = devices.filter(function (d) { return d.kind == "audioinput"; });
                    window.knVoiceInputOptions = inputs.length ? Object.fromEntries(inputs.map(function (d) {
                        return [d.deviceId, d.label || "Unknown Device"];
                    })) : {"default": "Default"};
                });
            }
        };
        window.knVoiceInputs();
    }
    return stringToNewUTF8(JSON.stringify(window.knVoiceInputOptions));
});

// The page's rate -> 48 kHz mono, then the recorder (miniaudio's linear resampler, one per rate).
static void web_drain(void) {
    static ma_linear_resampler resampler;
    static int resampler_rate;
    float in[4096], out[4096 * 2];
    const int rate = voice_web_rate();
    if (rate <= 0) return;
    if (rate != resampler_rate) {
        if (resampler_rate) ma_linear_resampler_uninit(&resampler, NULL);
        resampler_rate = 0;
        if (rate != VOICE_SAMPLE_RATE) {
            const ma_linear_resampler_config config = ma_linear_resampler_config_init(ma_format_f32, 1, (ma_uint32) rate, VOICE_SAMPLE_RATE);
            if (ma_linear_resampler_init(&config, NULL, &resampler) != MA_SUCCESS) return;
        }
        resampler_rate = rate;
    }
    for (int n; (n = voice_web_take_pcm(in, 4096)) > 0;) {
        if (rate == VOICE_SAMPLE_RATE) {
            rec_append(in, (size_t) n);
            continue;
        }
        ma_uint64 frames_in = (ma_uint64) n, frames_out = sizeof(out) / sizeof(out[0]);
        ma_linear_resampler_process_pcm_frames(&resampler, in, &frames_in, out, &frames_out);
        rec_append(out, (size_t) frames_out);
    }
}
#endif

// The recording indicator: v7.1.4's #voiceIcon toggles .voiceOn / .voiceOff (toggleRecord);
// v3.1.1's #voiceDisplay opacity 0.9 / 0.5 maps onto them.
static void voice_display_opacity(const char *opacity) {
    const int on = opacity && !strcmp(opacity, "0.9");
    const HtmlElem icon = html_dom_by_id("voiceIcon");
    if (icon) {
        html_dom_set_class(icon, "voiceOn", on);
        html_dom_set_class(icon, "voiceOff", !on);
    }
    const HtmlElem el = html_dom_by_id("voiceDisplay");
    if (el) html_dom_set_style(el, "opacity", opacity);
}

static int voice_echo_enabled(void) {
    const char *v = getenv("KRUNKNATIVE_VOICE_ECHO");
    return v && *v && strcmp(v, "0") != 0;
}

static int voice_send_packet(void *user, const NetPacketType type, const void *payload, const uint16_t length) {
    client_net_send(user, type, payload, length);
    return 1;
}

// KRUNKNATIVE_VOICE_DUMP: the file inside a clip we send, as <dir>/sent-<n>.<webm|ogg|wav>.
static void dump_sent(const char *url, const uint32_t seq) {
    const char *dir = getenv("KRUNKNATIVE_VOICE_DUMP");
    if (!dir || !*dir) return;
    size_t n = 0;
    uint8_t *data = data_url_decode(url, &n);
    if (!data) return;
    const char *ext = n >= 4 && !memcmp(data, "\x1A\x45\xDF\xA3", 4) ? "webm" : n >= 4 && !memcmp(data, "OggS", 4) ? "ogg" : "wav";
    char path[512];
    snprintf(path, sizeof(path), "%s/sent-%03u.%s", dir, seq, ext);
    FILE *file = fopen(path, "wb");
    if (file) {
        fwrite(data, 1, n, file);
        fclose(file);
    }
    free(data);
}

// blob "load": io.send("vc", dataUrl)
static void send_clip(Client *client, const char *url, const size_t len, const int frames) {
    voice_log("send clip %u: %d frames, %zu bytes", g_rec.seq, frames, len);
    dump_sent(url, g_rec.seq);
    if (social_server_v7(client)) { // v7: io.send("vc", dataUrl, vLN02023, micVolume, micQuality)
        const VoiceV7Send meta = {.channel = hud_ui_chat_channel(), .volume = g_vset.mic_volume, .quality = g_vset.mic_quality};
        voice_send_clip_v7(url, len, g_rec.seq++, &meta, voice_send_packet, client);
    } else {
        voice_send_clip(url, len, g_rec.seq++, voice_send_packet, client);
    }
    if (voice_echo_enabled() && client && client->me) voice_chat(client, url, client->me->name, client->me->uid);
}

static void recorder_start(void) {
    g_rec.recorder_on = 1;
}

// One recorder run (voiceRate ms) -> a clip. Native and web alike: samples drained from the
// microphone (native ring / the page's tap) are encoded here.
static void recorder_stop(Client *client) {
    if (!g_rec.recorder_on) return;
#ifdef __EMSCRIPTEN__
    web_drain();
#else
    capture_drain();
#endif
    g_rec.recorder_on = 0;
    // The recorder ran voiceRate ms of sample time: what the microphone delivered past that
    // (this frame came late) belongs to the next run, so clips cover the stream like
    // Chrome's timer-driven stop()/start() and not the frame pacing.
    const size_t limit = (size_t) VOICE_SAMPLE_RATE * VOICE_RATE_MS / 1000;
    const size_t n = g_rec.samples < limit ? g_rec.samples : limit;
    // KrunkNative fix: only whole 60 ms Opus frames reach a clip; the remainder of a run (20 ms of
    // every 500 ms) stays for the next one instead of being thrown away (a click every half second)
    const size_t used = opus_available() ? n / VOICE_FRAME_SAMPLES * VOICE_FRAME_SAMPLES : n;
    size_t len = 0;
    int frames = 0;
    char *url = used ? clip_encode(g_rec.pcm, used, &len, &frames) : NULL;
    memmove(g_rec.pcm, g_rec.pcm + used, (g_rec.samples - used) * sizeof(float));
    g_rec.samples -= used;
    if (url && frames > 0) send_clip(client, url, len, frames);
    else if (used && !url) voice_unavailable("the voice encoder failed");
    free(url);
}

static void session_end(void) {
    g_rec.session = 0;
    g_rec.samples = 0;
    g_status.level = 0.0f;
    const HtmlElem icon = html_dom_by_id("voiceIcon");
    if (icon) html_dom_set_style(icon, "opacity", "");
#ifdef __EMSCRIPTEN__
    voice_web_capture(0); // the stream stays open, like the page's MediaRecorder stream did
#endif
    // native: the capture device stays open (capture_open), like the page's stream
}

// getUserMedia(...).then: recorder started, interval running.
static void session_opened(void) {
    g_rec.pending = 0;
    g_rec.ready = 0;
    voice_display_opacity("0.9");
    g_rec.recording = 1;
    g_rec.session = 1;
    g_rec.samples = 0;
    g_status.session_start = g_status.loud_at = glfwGetTime();
    g_status.session_samples = 0;
    g_status.warned_silent = 0;
    g_status.last_error[0] = 0;
    if (!(g_vset.mic_volume > 0.0f) && !g_status.warned_mic_volume) {
        g_status.warned_mic_volume = 1; // the Pro preset sets micVolume 0: every receiver drops our clips
        voice_notice("Voice: your Mic Volume setting is 0, nobody will hear you (Settings > Audio)");
    }
    recorder_start();
    g_rec.next_tick = glfwGetTime() + VOICE_RATE_MS / 1000.0;
#ifdef __EMSCRIPTEN__
    voice_web_capture(1);
#endif
}

// .catch: no microphone (capture_open / the page said why: voice_unavailable).
static void session_refused(void) {
    g_rec.pending = 0;
    g_rec.recording = 0;
    voice_display_opacity("0.5");
    session_end();
    voice_log("microphone unavailable");
}

void voice_toggle_record(Client *client, int down) {
    if (ui_text_input_focused()) return;       // document.activeElement.tagName != "INPUT" (+ any text field)
    // v3.1.1 `player || window.spectating`; v7 has no such check (its server drops a clip without a
    // player). KrunkNative: a dead player (client->me is kept) and a spectator may talk.
    if (!client || !(client->me || spectate_active())) return;
    // v7 voiceChatTrigger 0 (Press / Toggle): a press flips the recording, a release keeps it
    if (g_vset.trigger == 0) down = down ? !g_rec.recording : g_rec.recording;
    if (down && !g_rec.recording && g_rec.ready) {
        if (g_rec.pending) return;
        voice_log("record");
#ifdef __EMSCRIPTEN__
        g_rec.pending = 1;
        voice_web_open(g_rec.input);
#else
        if (capture_open()) session_opened();
        else session_refused();
#endif
    } else if (!down && g_rec.recording && g_rec.session) {
        voice_log("release");
        g_rec.recording = 0;
    }
}

int voice_recording(void) {
    return g_rec.recording;
}

void voice_update(Client *client) {
    g_voice_client = client;
#ifdef __EMSCRIPTEN__
    static int inputs_listed;
    if (!inputs_listed) {
        inputs_listed = 1;
        free(voice_web_inputs()); // window.updateAudioInputs() on page load
    }
    if (g_rec.pending) {
        const int state = voice_web_state();
        char *error = state >= 2 ? voice_web_error() : NULL;
        if (error && error[0] == '\x01') voice_notice("Voice: the saved input device is gone, using the default microphone");
        if (state == 2) {
            session_opened();
        } else if (state == 3) {
            voice_unavailable(error && error[0] && error[0] != '\x01' ? error : "the microphone could not be opened");
            session_refused();
        }
        free(error);
    }
    if (g_rec.session) web_drain();
#else
    if (g_rec.session) capture_drain();
#endif
    if (g_rec.session) {
        voice_check_session();
        voice_show_level();
    }

    // cA = setInterval(..., config.voiceRate)
    if (g_rec.session && glfwGetTime() >= g_rec.next_tick) {
        g_rec.next_tick += VOICE_RATE_MS / 1000.0;
        if (g_rec.next_tick < glfwGetTime()) g_rec.next_tick = glfwGetTime() + VOICE_RATE_MS / 1000.0; // a long stall
        recorder_stop(client);
        if (g_rec.recording) {
            recorder_start();
        } else {
            voice_display_opacity("0.5");
            g_rec.ready = 1;
            session_end();
        }
    }

    voice_poll_loading();
    instances_update();
}

void voice_set_audio_input(const char *device_id) {
    snprintf(g_rec.input, sizeof(g_rec.input), "%s", device_id ? device_id : "");
#ifdef __EMSCRIPTEN__
    free(voice_web_inputs()); // updateAudioInputs()
#endif
}

char *voice_audio_inputs_json(void) {
#ifdef __EMSCRIPTEN__
    return voice_web_inputs();
#else
    // Natively there is no permission gate: the backend's capture devices are listed (device
    // name = option value, which audioInput persists, and label). Only the UI scenario renderer
    // (tools/uidiff) shows what the reference Chrome captures show before any permission.
#if defined(KRUNKNATIVE_HTML_ENGINE)
    if (ui_scenario_active()) {
        static const char UNKNOWN[] = "{\"\":\"Unknown Device\"}";
        char *copy = malloc(sizeof(UNKNOWN));
        if (copy) memcpy(copy, UNKNOWN, sizeof(UNKNOWN));
        return copy;
    }
#endif
    cJSON *options = cJSON_CreateObject();
    if (!options) return NULL;
    ma_device_info *infos = NULL;
    ma_uint32 count = 0;
    if (capture_context() && ma_context_get_devices(&g_cap.context, NULL, NULL, &infos, &count) == MA_SUCCESS && count) {
        // Chrome's "default" entry, labelled with the device it stands for.
        char label[300] = "Default";
        for (ma_uint32 i = 0; i < count; i++) {
            if (infos[i].isDefault) snprintf(label, sizeof(label), "Default - %s", infos[i].name);
        }
        cJSON_AddStringToObject(options, "default", label);
        for (ma_uint32 i = 0; i < count; i++) {
            if (!cJSON_GetObjectItemCaseSensitive(options, infos[i].name)) cJSON_AddStringToObject(options, infos[i].name, infos[i].name);
        }
    } else {
        cJSON_AddStringToObject(options, "default", "Default");
    }
    char *json = cJSON_PrintUnformatted(options);
    cJSON_Delete(options);
    return json;
#endif
}

void voice_fini(void) {
    voice_audio_fini();
    voice_reset_queues();
    social_assembly_free(&g_receive);
    social_assembly_free(&g_receive_v7);
    free(g_rec.pcm);
    g_rec.pcm = NULL;
    g_rec.samples = g_rec.cap = 0;
#ifndef __EMSCRIPTEN__
    capture_close();
    if (g_cap.rb_ready) ma_pcm_rb_uninit(&g_cap.rb);
    g_cap.rb_ready = 0;
    if (g_cap.context_ready) ma_context_uninit(&g_cap.context);
    g_cap.context_ready = 0;
#endif
}
