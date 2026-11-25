// K.P.D demo files (shared/include/demo.h, doc/v10/replays.md).
#include <demo.h>
#include <net_ext.h> // NET_EXT_KN_PLAYER_RANK_FLAG, NET_EXT_KN_PLAYER_TITLE, NET_EXT_KN_CHAT_NAMES
#include <social_v7.h> // NET_PACKET_CHAT_V7, NetChatV7Packet
#include <shared.h>
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int demo_buf_append(DemoBuf *buf, const void *data, const size_t length) {
    if (!length) return 1;
    if (buf->length + length > buf->capacity) {
        size_t cap = buf->capacity ? buf->capacity : 4096;
        while (cap < buf->length + length) cap *= 2;
        uint8_t *grown = realloc(buf->data, cap);
        if (!grown) return 0;
        buf->data = grown;
        buf->capacity = cap;
    }
    memcpy(buf->data + buf->length, data, length);
    buf->length += length;
    return 1;
}

void demo_buf_free(DemoBuf *buf) {
    free(buf->data);
    *buf = (DemoBuf) {0};
}

static void put_u32(uint8_t *p, const uint32_t v) {
    p[0] = (uint8_t) v;
    p[1] = (uint8_t) (v >> 8);
    p[2] = (uint8_t) (v >> 16);
    p[3] = (uint8_t) (v >> 24);
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}

int demo_write_header(DemoBuf *out, const char *meta_json) {
    const size_t meta_len = meta_json ? strlen(meta_json) : 0;
    if (meta_len > DEMO_META_MAX) return 0;
    uint8_t head[DEMO_HEADER_SIZE];
    memcpy(head, DEMO_MAGIC, DEMO_MAGIC_SIZE);
    head[DEMO_MAGIC_SIZE] = DEMO_VERSION;
    put_u32(head + DEMO_MAGIC_SIZE + 1, (uint32_t) meta_len);
    return demo_buf_append(out, head, sizeof(head)) && demo_buf_append(out, meta_json, meta_len);
}

int demo_write_record(DemoBuf *out, const uint32_t t_ms, const uint8_t kind, const uint8_t *bytes, const uint32_t length) {
    uint8_t head[DEMO_RECORD_HEADER_SIZE];
    put_u32(head, t_ms);
    head[4] = kind;
    put_u32(head + 5, length);
    return demo_buf_append(out, head, sizeof(head)) && demo_buf_append(out, bytes, length);
}

int demo_frames_each(uint8_t *bytes, const uint32_t length, const DemoPacketFn fn, void *user) {
    uint32_t at = 0;
    while (at < length) {
        if (length - at < NET_PACKET_HEADER_SIZE) return 0;
        const uint8_t type = bytes[at];
        const uint16_t n = (uint16_t) (bytes[at + 1] << 8 | bytes[at + 2]);
        if (n > NET_MAX_PAYLOAD || length - at - NET_PACKET_HEADER_SIZE < n) return 0;
        if (fn && !fn(user, type, bytes + at + NET_PACKET_HEADER_SIZE, n)) return 0;
        at += NET_PACKET_HEADER_SIZE + n;
    }
    return 1;
}

static void set_err(char *err, const size_t size, const char *text) {
    if (err && size) snprintf(err, size, "%s", text);
}

int demo_parse(Demo *demo, const uint8_t *data, const size_t length, char *err, const size_t err_size) {
    memset(demo, 0, sizeof(*demo));
    if (!data || length < DEMO_HEADER_SIZE || memcmp(data, DEMO_MAGIC, DEMO_MAGIC_SIZE) != 0) {
        set_err(err, err_size, "not a demo file");
        return 0;
    }
    if (data[DEMO_MAGIC_SIZE] < 1 || data[DEMO_MAGIC_SIZE] > DEMO_VERSION) {
        set_err(err, err_size, "unsupported demo version");
        return 0;
    }
    if (length > DEMO_MAX_BYTES) {
        set_err(err, err_size, "demo too large");
        return 0;
    }
    const uint32_t meta_len = get_u32(data + DEMO_MAGIC_SIZE + 1);
    if (meta_len > DEMO_META_MAX || meta_len > length - DEMO_HEADER_SIZE) {
        set_err(err, err_size, "truncated demo meta");
        return 0;
    }
    demo->data = malloc(length);
    demo->meta = malloc(meta_len + 1);
    if (!demo->data || !demo->meta) goto oom;
    memcpy(demo->data, data, length);
    demo->length = length;
    demo->version = data[DEMO_MAGIC_SIZE];
    memcpy(demo->meta, data + DEMO_HEADER_SIZE, meta_len);
    demo->meta[meta_len] = 0;

    size_t capacity = 0;
    size_t at = DEMO_HEADER_SIZE + meta_len;
    uint32_t last_t = 0;
    while (at < length) {
        if (length - at < DEMO_RECORD_HEADER_SIZE) {
            set_err(err, err_size, "truncated demo record");
            goto fail;
        }
        DemoRecord r = {.t_ms = get_u32(demo->data + at), .kind = demo->data[at + 4], .length = get_u32(demo->data + at + 5)};
        at += DEMO_RECORD_HEADER_SIZE;
        if (r.length > length - at || r.kind > DEMO_REC_KEYFRAME || r.t_ms < last_t) {
            set_err(err, err_size, "bad demo record");
            goto fail;
        }
        r.bytes = demo->data + at;
        if (!demo_frames_each(r.bytes, r.length, NULL, NULL)) {
            set_err(err, err_size, "bad demo frames");
            goto fail;
        }
        if (!demo->count && r.kind != DEMO_REC_KEYFRAME) {
            set_err(err, err_size, "demo has no keyframe"); // Krunker: "Demo has no keyframe" (start, :201849)
            goto fail;
        }
        if (demo->count == capacity) {
            capacity = capacity ? capacity * 2 : 256;
            DemoRecord *grown = realloc(demo->records, capacity * sizeof(*grown));
            if (!grown) goto oom;
            demo->records = grown;
        }
        demo->records[demo->count++] = r;
        last_t = r.t_ms;
        at += r.length;
    }
    if (!demo->count) {
        set_err(err, err_size, "demo has no keyframe");
        goto fail;
    }
    demo->duration_ms = last_t;
    return 1;
oom:
    set_err(err, err_size, "out of memory");
fail:
    demo_free(demo);
    return 0;
}

void demo_free(Demo *demo) {
    free(demo->data);
    free(demo->meta);
    free(demo->records);
    memset(demo, 0, sizeof(*demo));
}

int demo_first_keyframe(const Demo *demo) {
    for (size_t i = 0; i < demo->count; i++) {
        if (demo->records[i].kind == DEMO_REC_KEYFRAME) return (int) i;
    }
    return -1;
}

int demo_keyframe_at(const Demo *demo, const uint32_t t_ms) {
    int found = -1;
    for (size_t i = 0; i < demo->count && demo->records[i].t_ms <= t_ms; i++) {
        if (demo->records[i].kind == DEMO_REC_KEYFRAME) found = (int) i;
    }
    return found;
}

// ------------------------------------------------------------------ suspect segments

typedef struct {
    int32_t uid;
    int seen;    // a keyframe: the suspect's SPAWN was in it
    int present; // in the roster
    int alive;
} SuspectScan;

static int32_t read_i32(const uint8_t *p) {
    int32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static int suspect_packet(void *user, const uint8_t type, uint8_t *payload, const uint16_t length) {
    SuspectScan *s = user;
    if (type == NET_PACKET_SPAWN && length >= offsetof(NetSpawnPacket, active) + 1 && read_i32(payload) == s->uid) {
        s->seen = s->present = 1;
        s->alive = payload[offsetof(NetSpawnPacket, active)] != 0;
    } else if (type == NET_PACKET_KILL && length >= sizeof(int32_t) && read_i32(payload + offsetof(NetKillPacket, victim_uid)) == s->uid) {
        s->alive = 0;
    } else if (type == NET_PACKET_LEAVE && length >= sizeof(int32_t) && read_i32(payload) == s->uid) {
        s->present = 0;
    }
    return 1;
}

size_t demo_suspect_segments(const Demo *demo, const int32_t suspect_uid, DemoSegment *out, const size_t max) {
    if (!max) return 0;
    const uint32_t duration = demo->duration_ms;
    if (suspect_uid < 0 || !duration) {
        out[0] = (DemoSegment) {0, duration};
        return 1;
    }
    DemoSegment spans[DEMO_SEGMENTS_MAX * 4];
    size_t count = 0;
    SuspectScan s = {.uid = suspect_uid, .alive = 1};
    int open = 0;
    uint32_t open_at = 0;
    for (size_t i = 0; i <= demo->count; i++) {
        uint32_t t = duration;
        if (i < demo->count) {
            const DemoRecord *r = &demo->records[i];
            t = r->t_ms;
            if (r->kind == DEMO_REC_KEYFRAME) {
                s.seen = 0;
                demo_frames_each(r->bytes, r->length, suspect_packet, &s);
                s.present = s.seen; // the keyframe's roster decides presence (Krunker `_kf`)
            } else {
                demo_frames_each(r->bytes, r->length, suspect_packet, &s);
            }
        } else {
            s.present = 0; // the end closes an open span
        }
        if (s.present && s.alive) {
            if (!open) open = 1, open_at = t;
        } else if (open) {
            open = 0;
            if (t > open_at && count < sizeof(spans) / sizeof(spans[0])) spans[count++] = (DemoSegment) {open_at, t};
        }
    }
    if (!count) {
        out[0] = (DemoSegment) {0, duration};
        return 1;
    }
    size_t n = 0;
    out[n++] = spans[0];
    for (size_t i = 1; i < count; i++) {
        if (spans[i].start - out[n - 1].end < DEMO_SEGMENT_MERGE_MS) out[n - 1].end = spans[i].end;
        else if (n < max) out[n++] = spans[i];
        else out[n - 1].end = spans[i].end; // out of room: the last span reaches on
    }
    return n;
}

uint32_t demo_clamp_to_segments(const DemoSegment *segments, const size_t count, const uint32_t t_ms) {
    if (!segments || !count) return t_ms;
    uint32_t best = t_ms, best_gap = UINT32_MAX;
    for (size_t i = 0; i < count; i++) {
        if (t_ms >= segments[i].start && t_ms <= segments[i].end) return t_ms;
        const uint32_t edge = t_ms < segments[i].start ? segments[i].start : segments[i].end;
        const uint32_t gap = edge > t_ms ? edge - t_ms : t_ms - edge;
        if (gap < best_gap) best_gap = gap, best = edge;
    }
    return best;
}

// ------------------------------------------------------------------ identities

static void player_name(char *out, const size_t size, const int32_t uid) {
    memset(out, 0, size);
    snprintf(out, size, "Player_%d", (int) uid);
}

// hideIdentities' chat rule (deob-10.0.7.js:201759-201781): the rosters give an account -> sid map (roster
// [31] accid -> [1] sid, a later entry overwrites, a falsy accid is left out), then every "ch" frame's sender
// name becomes Player_<sid> of its account (data[6]), or plain "Player" when no roster had that account (a
// server line's data[6] is unset, a guest's 0), and data[6] becomes 0. The text (data[2]) is never touched,
// so a server line keeps any name written into it. Here the accounts come from PLAYER_INFO (uid, account_id).
#define DEMO_ACCOUNTS_MAX 512
typedef struct {
    int32_t account[DEMO_ACCOUNTS_MAX], uid[DEMO_ACCOUNTS_MAX];
    size_t count;
} DemoAccounts;

static int collect_account(void *user, const uint8_t type, uint8_t *payload, const uint16_t length) {
    DemoAccounts *a = user;
    if (type != NET_PACKET_PLAYER_INFO || length < sizeof(NetPlayerInfoPacket)) return 1;
    NetPlayerInfoPacket info;
    memcpy(&info, payload, sizeof(info));
    if (!info.account_id) return 1;
    for (size_t i = 0; i < a->count; i++) {
        if (a->account[i] == info.account_id) {
            a->uid[i] = info.uid;
            return 1;
        }
    }
    if (a->count < DEMO_ACCOUNTS_MAX) {
        a->account[a->count] = info.account_id;
        a->uid[a->count++] = info.uid;
    }
    return 1;
}

// A NET_PACKET_CHAT names its sender by uid, not by account: the account is the one that uid's PLAYER_INFO gave
static int32_t uid_account(const DemoAccounts *a, const int32_t uid) {
    for (size_t i = 0; uid >= 0 && i < a->count; i++)
        if (a->uid[i] == uid) return a->account[i];
    return 0;
}

static void chat_sender_name(const DemoAccounts *a, const int32_t account, char *name, const size_t size) {
    for (size_t i = 0; account && i < a->count; i++) {
        if (a->account[i] == account) {
            player_name(name, size, a->uid[i]);
            return;
        }
    }
    memset(name, 0, size);
    snprintf(name, size, "Player");
}

static int anonymize_packet(void *user, const uint8_t type, uint8_t *payload, const uint16_t length) {
    const DemoAccounts *accounts = user;
    if (type == NET_PACKET_SPAWN && length >= offsetof(NetSpawnPacket, name) + sizeof(((NetSpawnPacket *) 0)->name)) {
        player_name((char *) payload + offsetof(NetSpawnPacket, name), sizeof(((NetSpawnPacket *) 0)->name), read_i32(payload));
    } else if (type == NET_PACKET_CHAT && length >= offsetof(NetChatPacket, text)) {
        // every "ch", a server line (uid -1) as well
        const int32_t uid = read_i32(payload + offsetof(NetChatPacket, uid));
        chat_sender_name(accounts, uid_account(accounts, uid), (char *) payload + offsetof(NetChatPacket, name),
                         sizeof(((NetChatPacket *) 0)->name));
    } else if (type == NET_PACKET_SCRIPT_CHAT && length >= offsetof(NetScriptChatPacket, chat) + offsetof(NetChatPacket, text)) {
        // GAME.CHAT's line, which v10 sends as "ch" -1, null, text, 2 (deob-10.0.7.js:185742-185745)
        uint8_t *chat = payload + offsetof(NetScriptChatPacket, chat);
        const int32_t uid = read_i32(chat + offsetof(NetChatPacket, uid));
        chat_sender_name(accounts, uid_account(accounts, uid), (char *) chat + offsetof(NetChatPacket, name),
                         sizeof(((NetChatPacket *) 0)->name));
    } else if (type == NET_PACKET_CHAT_V7 && length >= offsetof(NetChatV7Packet, chat) + offsetof(NetChatPacket, text)) {
        // the v7 "ch" carries the account itself (accountId, v10's data[6]), which goes to 0 too
        const int32_t account = read_i32(payload + offsetof(NetChatV7Packet, account_id));
        uint8_t *chat = payload + offsetof(NetChatV7Packet, chat);
        chat_sender_name(accounts, account, (char *) chat + offsetof(NetChatPacket, name), sizeof(((NetChatPacket *) 0)->name));
        memset(payload + offsetof(NetChatV7Packet, account_id), 0, sizeof(int32_t));
    } else if (type == NET_PACKET_PLAYER_INFO && length >= sizeof(NetPlayerInfoPacket)) {
        NetPlayerInfoPacket info;
        memcpy(&info, payload, sizeof(info));
        info.account_id = 0;
        info.level = 0;
        info.flags &= NET_PLAYER_INFO_BOT;
        memset(info.clan, 0, sizeof(info.clan));
        memcpy(payload, &info, sizeof(info));
    } else if (type == NET_PACKET_EXT && length == NET_EXT_HEADER_SIZE + sizeof(NetPlayerRankFlagPacket) &&
               (payload[0] | payload[1] << 8) == NET_EXT_KN_PLAYER_RANK_FLAG) {
        // the account's rank and Country Flag go with the rest of the account
        NetPlayerRankFlagPacket rank_flag;
        memcpy(&rank_flag, payload + NET_EXT_HEADER_SIZE, sizeof(rank_flag));
        rank_flag.ranked_rank = 0;
        rank_flag.flag = -1;
        memcpy(payload + NET_EXT_HEADER_SIZE, &rank_flag, sizeof(rank_flag));
    } else if (type == NET_PACKET_EXT && length == NET_EXT_HEADER_SIZE + sizeof(NetPlayerTitlePacket) &&
               (payload[0] | payload[1] << 8) == NET_EXT_KN_PLAYER_TITLE) {
        // and its worn title (ADD_PLAYERS' [52], -1 under anonMode as well)
        NetPlayerTitlePacket title;
        memcpy(&title, payload + NET_EXT_HEADER_SIZE, sizeof(title));
        title.title_index = -1;
        memcpy(payload + NET_EXT_HEADER_SIZE, &title, sizeof(title));
    } else if (type == NET_PACKET_EXT && length == NET_EXT_HEADER_SIZE + sizeof(NetChatNamesPacket) &&
               (payload[0] | payload[1] << 8) == NET_EXT_KN_CHAT_NAMES) {
        // a bot's "ch" line naming a player: its sender as any "ch" sender; the player it names is part of the
        // line's text, which v10 leaves alone
        uint8_t *sender = payload + NET_EXT_HEADER_SIZE + offsetof(NetChatNamesPacket, sender);
        const int32_t uid = read_i32(sender + offsetof(NetServerNameEntry, uid));
        chat_sender_name(accounts, uid_account(accounts, uid), (char *) sender + offsetof(NetServerNameEntry, name),
                         sizeof(((NetServerNameEntry *) 0)->name));
    }
    return 1;
}

void demo_anonymize(Demo *demo) {
    DemoAccounts *accounts = calloc(1, sizeof(*accounts));
    if (!accounts) return;
    // the accounts first: a chat line can come before its sender's PLAYER_INFO (v10 reads every roster first)
    for (size_t i = 0; i < demo->count; i++) demo_frames_each(demo->records[i].bytes, demo->records[i].length, collect_account, accounts);
    for (size_t i = 0; i < demo->count; i++) demo_frames_each(demo->records[i].bytes, demo->records[i].length, anonymize_packet, accounts);
    free(accounts);
}

int32_t demo_resolve_suspect(const Demo *demo, const char *call_token) {
    cJSON *meta = demo->meta ? cJSON_Parse(demo->meta) : NULL;
    int32_t uid = -1;
    const cJSON *calls = cJSON_GetObjectItemCaseSensitive(meta, "calls");
    const cJSON *call;
    cJSON_ArrayForEach(call, calls) {
        const cJSON *token = cJSON_GetObjectItemCaseSensitive(call, "token");
        const cJSON *suspect = cJSON_GetObjectItemCaseSensitive(call, "suspect");
        if (call_token && call_token[0] && cJSON_IsString(token) && !strcmp(token->valuestring, call_token) && cJSON_IsNumber(suspect)) {
            uid = (int32_t) suspect->valuedouble;
            break;
        }
    }
    if (uid < 0) {
        const cJSON *first = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(meta, "suspects"), 0);
        if (cJSON_IsNumber(first)) uid = (int32_t) first->valuedouble;
    }
    cJSON_Delete(meta);
    return uid;
}
