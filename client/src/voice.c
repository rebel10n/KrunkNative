// Client voice chat (client/include/voice_chat.h): Krunker v3.1.1 src/index.js
//   toggleRecord   push to talk: getUserMedia, MediaRecorder restarted every voiceRate, each
//                  stop -> readAsDataURL -> io.send("vc", dataUrl); #voiceDisplay opacity
//   voiceChat      "vc" from the server: dropped while the voice volume is 0
//   addVoiceStream one Howl per clip in voiceStreamQueues[alias]; plays when loaded, first in
//                  its queue and its player is known; longer than voiceChatMaxLength * 1.2: gone
//   remSpeaker     a clip ended: next one in the queue plays
// The protocol / server side is shared/src/voice.c.
#include <voice_chat.h>
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
#elif defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define VOICE_FRAME_SAMPLES (VOICE_SAMPLE_RATE * VOICE_FRAME_MS / 1000) // 2880
#define VOICE_MAX_INSTANCES 32
#define VOICE_MAX_SPEAKERS 64
#define VOICE_MAX_QUEUE 1024           // per speaker (v3.1.1: unbounded)
#define VOICE_NAME_MAX 64
#define VOICE_CAPTURE_FRAMES (VOICE_SAMPLE_RATE * 2) // ring buffer: 2 s
#define VOICE_FALLBACK_RATE 8000       // WAV clips without libopus
#define VOICE_ICON_MS 300.0            // overlay ah: speaker icon frame

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
    printf("[voice] t=%.3f ", glfwGetTime());
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

// ---------------------------------------------------------------- libopus (run-time loaded)

typedef struct OpusEncoder OpusEncoder;
typedef struct OpusDecoder OpusDecoder;
#define OPUS_APPLICATION_AUDIO 2049 // Chrome's AudioTrackOpusEncoder
#define OPUS_SET_BITRATE_REQUEST 4002

static struct {
    int tried, ok;
    void *lib;
    OpusEncoder *(*encoder_create)(int32_t fs, int channels, int application, int *error);
    int (*encoder_ctl)(OpusEncoder *, int request, ...);
    int32_t (*encode_float)(OpusEncoder *, const float *pcm, int frame_size, unsigned char *data, int32_t max_bytes);
    void (*encoder_destroy)(OpusEncoder *);
    OpusDecoder *(*decoder_create)(int32_t fs, int channels, int *error);
    int (*decode_float)(OpusDecoder *, const unsigned char *data, int32_t len, float *pcm, int frame_size, int fec);
    void (*decoder_destroy)(OpusDecoder *);
} g_opus;

#ifndef __EMSCRIPTEN__
static void *opus_symbol(const char *name) {
#ifdef _WIN32
    return (void *) GetProcAddress((HMODULE) g_opus.lib, name);
#else
    return dlsym(g_opus.lib, name);
#endif
}

static void *opus_open(const char *path) {
#ifdef _WIN32
    return (void *) LoadLibraryA(path);
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}
#endif

static int opus_available(void) {
#ifdef __EMSCRIPTEN__
    return 0; // the browser encodes (MediaRecorder) and decodes (decodeAudioData)
#else
    if (g_opus.tried) return g_opus.ok;
    g_opus.tried = 1;

    const char *forced = getenv("KRUNKNATIVE_OPUS");
    if (forced && !strcmp(forced, "0")) return 0;
    static const char *const NAMES[] = {
#ifdef _WIN32
        "opus.dll", "libopus-0.dll", "libopus.dll",
#elif defined(__APPLE__)
        "libopus.0.dylib", "libopus.dylib", "/opt/homebrew/lib/libopus.0.dylib", "/usr/local/lib/libopus.0.dylib",
#else
        "libopus.so.0", "libopus.so",
#endif
    };
    if (forced && *forced) g_opus.lib = opus_open(forced);
    for (size_t i = 0; !g_opus.lib && i < sizeof(NAMES) / sizeof(NAMES[0]); i++) g_opus.lib = opus_open(NAMES[i]);
    if (!g_opus.lib) {
        fprintf(stderr, "[voice] libopus not found: voice clips are sent as WAV and Opus clips cannot be played\n");
        return 0;
    }

    *(void **) &g_opus.encoder_create = opus_symbol("opus_encoder_create");
    *(void **) &g_opus.encoder_ctl = opus_symbol("opus_encoder_ctl");
    *(void **) &g_opus.encode_float = opus_symbol("opus_encode_float");
    *(void **) &g_opus.encoder_destroy = opus_symbol("opus_encoder_destroy");
    *(void **) &g_opus.decoder_create = opus_symbol("opus_decoder_create");
    *(void **) &g_opus.decode_float = opus_symbol("opus_decode_float");
    *(void **) &g_opus.decoder_destroy = opus_symbol("opus_decoder_destroy");
    g_opus.ok = g_opus.encoder_create && g_opus.encoder_ctl && g_opus.encode_float && g_opus.encoder_destroy &&
        g_opus.decoder_create && g_opus.decode_float && g_opus.decoder_destroy;
    if (!g_opus.ok) fprintf(stderr, "[voice] libopus is incomplete, Opus disabled\n");
    return g_opus.ok;
#endif
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
    ebml_string(&info, 0x4D80, "KrunkNative"); // MuxingApp
    ebml_string(&info, 0x5741, "KrunkNative"); // WritingApp

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
        g_opus.encoder_ctl(encoder, OPUS_SET_BITRATE_REQUEST, (int32_t) VOICE_OPUS_BITRATE);
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
    float volume;       // Howl volume: master * voice * 2 at creation
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
    if (ma_sound_init_from_data_source(engine, &instance->buffer, MA_SOUND_FLAG_NO_SPATIALIZATION, NULL, &instance->sound) != MA_SUCCESS) {
        ma_audio_buffer_ref_uninit(&instance->buffer);
        rem_speaker(name, 0, 0);
        return;
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
        if (queue->count && queue->clips[0]->ready && queue->clips[0]->has_src) clip_play(queue->clips[0], name);
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
        if (queue && queue->count && queue->clips[0] == clip && clip->has_src) clip_play(clip, name);
    } else {
        rem_speaker(name, 0, 1);
    }
    if (!clip->has_src) {
        // Never playable (srcObj is fixed at arrival): v3.1.1 keeps the Howl; only the
        // queue entry matters (it blocks the queue just the same), not its samples.
        free(clip->pcm.pcm);
        clip->pcm.pcm = NULL;
        clip->pcm.frames = 0;
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
        clip_unref(clip); // the decode's reference
    }
#endif
}

// window.addVoiceStream(player, name, audio)
static void add_voice_stream(const int has_src, const char *name, const char *audio) {
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
    clip->volume = audio_get_volume(AUDIO_CAT_MASTER) * audio_get_volume(AUDIO_CAT_VOICE) * 2.0f;
    queue->clips[queue->count++] = clip;

#ifdef __EMSCRIPTEN__
    if (g_play.loading_count < (int) (sizeof(g_play.loading) / sizeof(g_play.loading[0]))) {
        clip->loading = 1;
        clip->web_id = voice_web_decode_start(audio);
        clip->refs++;
        g_play.loading[g_play.loading_count++] = clip;
    }
#else
    if (clip_decode(audio, &clip->pcm)) clip_loaded(clip);
    else voice_log("cannot decode clip from %s (%zu bytes)", name, strlen(audio)); // stays queued, never ready
#endif
}

// window.voiceChat(audio, name, sid)
static void voice_chat(Client *client, const char *audio, const char *name, const int sid) {
    if (!(audio_get_volume(AUDIO_CAT_VOICE) > 0.0f)) return; // if (SOUND.volumes[1])
    char scrambled[VOICE_NAME_MAX];
    name = streamer_name(name, scrambled, sizeof(scrambled)); // streamerModeEnabled && Filter: UTILS.scrambleS(hk)
    const Player *player = client && sid >= 0 ? game_find_player(&client->game, sid) : NULL;
    voice_log("recv %s sid=%d %zu bytes%s", name, sid, strlen(audio), player ? "" : " (unknown player)");
    add_voice_stream(player != NULL, name, audio);
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

void voice_on_packet(Client *client, const void *payload, const uint16_t length) {
    char *msg = NULL;
    uint32_t sid = 0;
    if (social_assembly_push(&g_receive, payload, length, &msg, &sid) != 1) return;
    char alias[VOICE_NAME_MAX];
    const char *audio = NULL;
    if (voice_parse_relay(msg, alias, sizeof(alias), &audio)) voice_chat(client, audio, alias, (int) (int32_t) sid);
    free(msg);
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

#ifndef __EMSCRIPTEN__
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
}

// getUserMedia({audio: audioInput != "default" ? {deviceId: {exact}} : true})
// The device stays open after a session, like v3.1.1's never-stopped getUserMedia stream;
// the next session reuses it unless the requested input changed (v3.1.1 asks getUserMedia
// again with the current audioInput every session).
static int capture_open(void) {
    const char *forced = getenv("KRUNKNATIVE_VOICE_INPUT"); // tests: a virtual microphone by name
    const char *input = forced && *forced ? forced : g_rec.input;
    if (g_cap.device_open && !strcmp(g_cap.opened_input, input)) {
        // Between sessions nobody reads the ring (the callback drops what does not fit):
        // whatever is in it is stale, the session records from now on.
        capture_discard();
        return 1;
    }
    capture_close();
    if (!capture_context()) return 0;
    if (!g_cap.rb_ready) {
        if (ma_pcm_rb_init(ma_format_f32, 1, VOICE_CAPTURE_FRAMES, NULL, NULL, &g_cap.rb) != MA_SUCCESS) return 0;
        g_cap.rb_ready = 1;
    }
    capture_discard(); // the audio thread is stopped here: nothing races the reader

    ma_device_config config = ma_device_config_init(ma_device_type_capture);
    config.capture.format = ma_format_f32;
    config.capture.channels = 1;
    config.sampleRate = VOICE_SAMPLE_RATE;
    config.dataCallback = capture_callback;

    ma_device_id id;
    if (input[0] && strcmp(input, "default") != 0) {
        ma_device_info *infos = NULL;
        ma_uint32 count = 0;
        if (ma_context_get_devices(&g_cap.context, NULL, NULL, &infos, &count) != MA_SUCCESS) return 0;
        int found = 0;
        for (ma_uint32 i = 0; i < count && !found; i++) {
            if (!strcmp(infos[i].name, input)) {
                id = infos[i].id;
                found = 1;
            }
        }
        if (!found) return 0; // deviceId: {exact}: OverconstrainedError
        config.capture.pDeviceID = &id;
    }

    if (ma_device_init(&g_cap.context, &config, &g_cap.device) != MA_SUCCESS) return 0;
    g_cap.device_open = 1;
    snprintf(g_cap.opened_input, sizeof(g_cap.opened_input), "%s", input);
    if (ma_device_start(&g_cap.device) != MA_SUCCESS) {
        capture_close();
        return 0;
    }
    voice_log("microphone open: %s", g_cap.device.capture.name);
    return 1;
}

static void capture_drain(void) {
    if (!g_cap.rb_ready) return;
    for (;;) {
        ma_uint32 n = ma_pcm_rb_available_read(&g_cap.rb);
        if (!n) return;
        void *src = NULL;
        if (ma_pcm_rb_acquire_read(&g_cap.rb, &n, &src) != MA_SUCCESS || !n) return;
        if (g_rec.recorder_on) {
            if (g_rec.samples + n > g_rec.cap) {
                size_t cap = g_rec.cap ? g_rec.cap : VOICE_SAMPLE_RATE;
                while (cap < g_rec.samples + n) cap *= 2;
                float *grown = realloc(g_rec.pcm, cap * sizeof(float));
                if (grown) {
                    g_rec.pcm = grown;
                    g_rec.cap = cap;
                }
            }
            if (g_rec.samples + n <= g_rec.cap) {
                memcpy(g_rec.pcm + g_rec.samples, src, n * sizeof(float));
                g_rec.samples += n;
            }
        }
        ma_pcm_rb_commit_read(&g_cap.rb, n);
    }
}
#else
// v3.1.1's capture, verbatim: getUserMedia -> one MediaRecorder per stream, started and
// stopped by C (voice_update), every stop -> Blob -> readAsDataURL -> a queued clip that C
// takes and sends. No UI logic here: C owns cB / cC / the interval / #voiceDisplay.
EM_JS(void, voice_web_open, (const char *device), {
    var v = window.knVoice = window.knVoice || {clips: [], decodes: {}, nextId: 1, state: 0};
    var id = UTF8ToString(device);
    v.state = 1;
    try {
        navigator.mediaDevices.getUserMedia({
            audio: id && id != "default" ? {deviceId: {exact: id}} : true,
            video: false
        }).then(function (stream) {
            if (window.knVoiceInputs) window.knVoiceInputs();
            // v3.1.1 keeps every earlier stream open as well; the microphone is just as open
            // with only the newest one, so the previous one is released instead of leaked.
            if (v.stream && v.stream !== stream) v.stream.getTracks().forEach(function (t) { t.stop(); });
            v.stream = stream;
            v.recorder = new MediaRecorder(stream, {mimetype: "audio/ogg", bitsPerSecond: 6000});
            var parts = [];
            v.recorder.addEventListener("dataavailable", function (e) { parts.push(e.data); });
            v.recorder.addEventListener("stop", function () {
                var blob = new Blob(parts, {type: "audio/ogg; codecs=opus"});
                parts.length = 0;
                var reader = new FileReader();
                reader.addEventListener("load", function () { v.clips.push(reader.result); });
                reader.readAsDataURL(blob);
            });
            v.state = 2;
        }).catch(function () { v.state = 3; });
    } catch (e) { v.state = 3; }
});

// 0 idle, 1 asking, 2 open, 3 refused
EM_JS(int, voice_web_state, (void), {
    return window.knVoice ? window.knVoice.state : 0;
});

// recorder.start() and cA = setInterval(..., config.voiceRate) on the page's own timer (the
// frame loop can run far slower than 2 Hz in a background tab): each tick stops the recorder
// (one clip) and starts it again while cB (`recording`) holds, else the session is over.
EM_JS(void, voice_web_session_start, (int rate_ms), {
    var v = window.knVoice, r = v && v.recorder;
    if (!r) return;
    v.recording = 1;
    v.ended = 0;
    if (r.state != "recording") r.start();
    v.timer = setInterval(function () {
        if (r.state == "recording") r.stop();
        if (v.recording) {
            r.start();
        } else {
            clearInterval(v.timer);
            v.timer = 0;
            v.ended = 1;
        }
    }, rate_ms);
});

EM_JS(void, voice_web_set_recording, (int on), {
    if (window.knVoice) window.knVoice.recording = on;
});

// 1 once the interval saw cB false (C then dims #voiceDisplay and sets cC).
EM_JS(int, voice_web_take_ended, (void), {
    var v = window.knVoice;
    if (!v || !v.ended) return 0;
    v.ended = 0;
    return 1;
});

EM_JS(void, voice_web_close, (void), {
    var v = window.knVoice;
    if (!v) return;
    // End of a session: the stream stays open (v3.1.1 never stops it, the microphone stays in
    // use after the first push to talk); the next session asks getUserMedia again.
    if (v.timer) clearInterval(v.timer);
    v.timer = 0;
    if (v.recorder && v.recorder.state == "recording") v.recorder.stop();
    v.state = 0;
});

EM_JS(char *, voice_web_take_clip, (void), {
    var v = window.knVoice;
    if (!v || !v.clips.length) return 0;
    return stringToNewUTF8(v.clips.shift());
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
    voice_send_clip(url, len, g_rec.seq++, voice_send_packet, client);
    if (voice_echo_enabled() && client && client->me) voice_chat(client, url, client->me->name, client->me->uid);
}

static void recorder_start(void) {
    g_rec.recorder_on = 1;
}

static void recorder_stop(Client *client) {
    if (!g_rec.recorder_on) return;
    g_rec.recorder_on = 0;
#ifdef __EMSCRIPTEN__
    (void) client; // the page's interval stops the recorder; clips come from voice_web_take_clip()
#else
    capture_drain();
    // The recorder ran voiceRate ms of sample time: what the microphone delivered past that
    // (this frame came late) belongs to the next run, so clips cover the stream like
    // Chrome's timer-driven stop()/start() and not the frame pacing.
    const size_t limit = (size_t) VOICE_SAMPLE_RATE * VOICE_RATE_MS / 1000;
    const size_t n = g_rec.samples < limit ? g_rec.samples : limit;
    size_t len = 0;
    int frames = 0;
    char *url = clip_encode(g_rec.pcm, n, &len, &frames);
    memmove(g_rec.pcm, g_rec.pcm + n, (g_rec.samples - n) * sizeof(float));
    g_rec.samples -= n;
    if (url) send_clip(client, url, len, frames);
    free(url);
#endif
}

static void session_end(void) {
    g_rec.session = 0;
    g_rec.samples = 0;
#ifdef __EMSCRIPTEN__
    voice_web_close();
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
    recorder_start();
    g_rec.next_tick = glfwGetTime() + VOICE_RATE_MS / 1000.0;
#ifdef __EMSCRIPTEN__
    voice_web_session_start(VOICE_RATE_MS);
#endif
}

// .catch: no microphone.
static void session_refused(void) {
    g_rec.pending = 0;
    g_rec.recording = 0;
    voice_display_opacity("0.5");
    session_end();
    voice_log("microphone unavailable");
}

void voice_toggle_record(Client *client, const int down) {
    if (ui_text_input_focused()) return;       // document.activeElement.tagName != "INPUT" (+ any text field)
    if (!client || !(client->me || spectate_active())) return; // player || window.spectating
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
#ifdef __EMSCRIPTEN__
        voice_web_set_recording(0);
#endif
    }
}

int voice_recording(void) {
    return g_rec.recording;
}

void voice_update(Client *client) {
#ifdef __EMSCRIPTEN__
    static int inputs_listed;
    if (!inputs_listed) {
        inputs_listed = 1;
        free(voice_web_inputs()); // window.updateAudioInputs() on page load
    }
    if (g_rec.pending) {
        const int state = voice_web_state();
        if (state == 2) session_opened();
        else if (state == 3) session_refused();
    }
    for (char *url; (url = voice_web_take_clip()) != NULL; free(url)) send_clip(client, url, strlen(url), -1);
    if (g_rec.session && voice_web_take_ended()) {
        voice_display_opacity("0.5");
        g_rec.ready = 1;
        session_end();
    }
#else
    if (g_rec.session) capture_drain();

    // cA = setInterval(..., config.voiceRate)
    if (g_rec.session && glfwGetTime() >= g_rec.next_tick) {
        g_rec.next_tick += VOICE_RATE_MS / 1000.0;
        recorder_stop(client);
        if (g_rec.recording) {
            recorder_start();
        } else {
            voice_display_opacity("0.5");
            g_rec.ready = 1;
            session_end();
        }
    }
#endif

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
