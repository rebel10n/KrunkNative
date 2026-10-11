// Krunker v3.1.1 voice chat relay (shared/include/voice.h): server/packetHandlers.ts io_vc.
#include <voice.h>
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
    const char *alias = speaker->alias ? speaker->alias : "";
    size_t alias_len = strlen(alias);
    if (alias_len > VOICE_ALIAS_MAX) alias_len = VOICE_ALIAS_MAX;
    const size_t msg_len = 1 + alias_len + audio_len;
    char *msg = malloc(msg_len);
    if (!msg) {
        free(audio);
        return 0;
    }
    msg[0] = (char) (unsigned char) alias_len;
    memcpy(msg + 1, alias, alias_len);
    memcpy(msg + 1 + alias_len, audio, audio_len);
    free(audio);

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
