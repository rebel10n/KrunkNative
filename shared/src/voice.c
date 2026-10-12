// Krunker v3.1.1 voice chat relay (shared/include/voice.h): server/packetHandlers.ts io_vc.
#include <voice.h>
#include <social_v7.h> // NET_PACKET_VOICE_V7
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int voice_send_clip(const char *audio, const size_t length, const uint32_t seq, const SocialSendFn send, void *user) {
    if (!audio || !length || length > VOICE_MAX_DATA || !send) return 0;
    return social_send_message(NET_PACKET_VOICE_SEND, seq, audio, length, send, user);
}

typedef struct {
    VoicePeerSendFn send;
    void *user;
    int peer;
} VoicePeerTarget;

static int voice_peer_send(void *user, const NetPacketType type, const void *payload, const uint16_t length) {
    const VoicePeerTarget *target = user;
    return target->send(target->user, target->peer, type, payload, length);
}

char *voice_relay_message(const char *alias, const char *audio, size_t *length) {
    if (!audio || !length) return NULL;
    alias = alias ? alias : "";
    size_t alias_len = strlen(alias);
    if (alias_len > VOICE_ALIAS_MAX) alias_len = VOICE_ALIAS_MAX;
    const size_t audio_len = strlen(audio);
    char *msg = malloc(1 + alias_len + audio_len + 1);
    if (!msg) return NULL;
    msg[0] = (char) (unsigned char) alias_len;
    memcpy(msg + 1, alias, alias_len);
    memcpy(msg + 1 + alias_len, audio, audio_len + 1);
    *length = 1 + alias_len + audio_len;
    return msg;
}

int voice_relay(SocialAssembly *assembly, const void *payload, const size_t length, const VoiceSpeaker *speaker,
                const int peer_count, const VoicePeerSendFn send, void *user) {
    char *audio = NULL;
    const int status = social_assembly_push(assembly, payload, length, &audio, NULL);
    if (status != 1) return status < 0 ? -1 : 0;

    // `if (!player || typeof audio !== "string" || !audio.length) return;` and the 256 KiB cap
    // (a string: the clip ends at its first NUL, like any C string here).
    const size_t audio_len = strlen(audio);
    if (!speaker || !speaker->has_player || !audio_len || audio_len > VOICE_MAX_DATA || !send) {
        free(audio);
        return 0;
    }

    // ["vc", audio, player.alias, player.sid]
    size_t msg_len = 0;
    char *msg = voice_relay_message(speaker->alias, audio, &msg_len);
    free(audio);
    if (!msg) return 0;

    int reached = 0;
    for (int peer = 0; peer < peer_count; peer++) {
        if (peer == speaker->peer) continue; // "Skip echoing back to the sender"
        VoicePeerTarget target = {send, user, peer};
        if (social_send_message(NET_PACKET_VOICE, (uint32_t) speaker->sid, msg, msg_len, voice_peer_send, &target)) reached++;
    }
    free(msg);
    return reached;
}

int voice_parse_relay(const char *msg, char *alias, const size_t alias_size, const char **audio) {
    if (!msg || !audio) return 0;
    const size_t alias_len = (unsigned char) msg[0];
    if (memchr(msg + 1, 0, alias_len)) return 0; // the alias is followed by the audio, not the end
    if (alias && alias_size) {
        const size_t n = alias_len < alias_size - 1 ? alias_len : alias_size - 1;
        memcpy(alias, msg + 1, n);
        alias[n] = 0;
    }
    *audio = msg + 1 + alias_len;
    return (*audio)[0] != 0;
}

// ---------------------------------------------------------------- v7.1.4 framing

int voice_send_clip_v7(const char *audio, const size_t length, const uint32_t seq, const VoiceV7Send *meta, const SocialSendFn send, void *user) {
    if (!audio || !length || length > VOICE_V7_MAX_DATA || !send || !meta) return 0;
    char header[VOICE_V7_HEADER_MAX];
    const int n = snprintf(header, sizeof(header), "%d %.3f %d\n", meta->channel, (double) meta->volume, meta->quality);
    if (n <= 0 || (size_t) n >= sizeof(header)) return 0;
    char *msg = malloc((size_t) n + length);
    if (!msg) return 0;
    memcpy(msg, header, (size_t) n);
    memcpy(msg + n, audio, length);
    const int ok = social_send_message((NetPacketType) NET_PACKET_VOICE_V7, seq, msg, (size_t) n + length, send, user);
    free(msg);
    return ok;
}

int voice_parse_send_v7(const char *msg, VoiceV7Send *meta, const char **audio) {
    if (!msg || !meta || !audio) return 0;
    const char *nl = strchr(msg, '\n');
    if (!nl || nl - msg >= VOICE_V7_HEADER_MAX) return 0;
    char header[VOICE_V7_HEADER_MAX];
    memcpy(header, msg, (size_t) (nl - msg));
    header[nl - msg] = 0;
    double channel = -1.0, volume = 0.0;
    int quality = 0;
    if (sscanf(header, "%lf %lf %d", &channel, &volume, &quality) < 2) return 0;
    // integer(args[1], -1); Math.max(0, Math.min(1, Number(args[2]) || 0))
    meta->channel = channel == (double) (int) channel ? (int) channel : -1;
    meta->volume = volume == volume ? (float) (volume < 0.0 ? 0.0 : volume > 1.0 ? 1.0 : volume) : 0.0f;
    meta->quality = quality;
    *audio = nl + 1;
    const size_t len = strlen(*audio);
    return len && len <= VOICE_V7_MAX_DATA && !strncmp(*audio, VOICE_V7_DATA_PREFIX, strlen(VOICE_V7_DATA_PREFIX));
}

char *voice_relay_v7_message(const VoiceV7Meta *meta, const char *alias, const char *audio, size_t *length) {
    if (!meta || !audio || !length) return NULL;
    alias = alias ? alias : "";
    size_t alias_len = strlen(alias);
    if (alias_len > VOICE_ALIAS_MAX) alias_len = VOICE_ALIAS_MAX;
    char header[VOICE_V7_HEADER_MAX];
    const int n = snprintf(header, sizeof(header), "%d %.3f %d %d %zu\n", meta->channel, (double) meta->volume, meta->is_final ? 1 : 0,
                           meta->account_id, alias_len);
    if (n <= 0 || (size_t) n >= sizeof(header)) return NULL;
    const size_t audio_len = strlen(audio);
    char *msg = malloc((size_t) n + alias_len + audio_len + 1);
    if (!msg) return NULL;
    memcpy(msg, header, (size_t) n);
    memcpy(msg + n, alias, alias_len);
    memcpy(msg + n + alias_len, audio, audio_len + 1);
    *length = (size_t) n + alias_len + audio_len;
    return msg;
}

int voice_parse_relay_v7(const char *msg, VoiceV7Meta *meta, char *alias, const size_t alias_size, const char **audio) {
    if (!msg || !meta || !audio) return 0;
    const char *nl = strchr(msg, '\n');
    if (!nl || nl - msg >= VOICE_V7_HEADER_MAX) return 0;
    char header[VOICE_V7_HEADER_MAX];
    memcpy(header, msg, (size_t) (nl - msg));
    header[nl - msg] = 0;
    int channel = -1, is_final = 0, account = 0;
    double volume = 0.0;
    size_t alias_len = 0;
    if (sscanf(header, "%d %lf %d %d %zu", &channel, &volume, &is_final, &account, &alias_len) != 5 || alias_len > VOICE_ALIAS_MAX) return 0;
    const char *name = nl + 1;
    if (memchr(name, 0, alias_len)) return 0; // the alias is followed by the audio, not the end
    meta->channel = channel;
    meta->volume = volume == volume ? (float) (volume < 0.0 ? 0.0 : volume > 1.0 ? 1.0 : volume) : 0.0f;
    meta->is_final = is_final != 0;
    meta->account_id = account;
    if (alias && alias_size) {
        const size_t n = alias_len < alias_size - 1 ? alias_len : alias_size - 1;
        memcpy(alias, name, n);
        alias[n] = 0;
    }
    *audio = name + alias_len;
    return (*audio)[0] != 0;
}
