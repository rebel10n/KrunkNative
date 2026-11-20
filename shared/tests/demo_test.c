// K.P.D demo files (shared/include/demo.h): write / parse round trip, determinism, keyframe lookup, the
// suspect segments and clamp (computeSuspectSegments / clampToSegments, deob-10.0.7.js:201693-201751),
// hideIdentities and the rejected files.
#include <demo.h>
#include <shared.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                                  \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

static void frame(DemoBuf *b, const uint8_t type, const void *payload, const uint16_t length) {
    const uint8_t head[3] = {type, (uint8_t) (length >> 8), (uint8_t) length};
    demo_buf_append(b, head, sizeof(head));
    demo_buf_append(b, payload, length);
}

static void spawn(DemoBuf *b, const int32_t uid, const int active, const char *name) {
    NetSpawnPacket s;
    memset(&s, 0, sizeof(s));
    s.uid = uid;
    s.active = (uint8_t) active;
    snprintf(s.name, sizeof(s.name), "%s", name);
    frame(b, NET_PACKET_SPAWN, &s, sizeof(s));
}

static void kill(DemoBuf *b, const int32_t victim, const int32_t killer) {
    NetKillPacket k;
    memset(&k, 0, sizeof(k));
    k.victim_uid = victim;
    k.killer_uid = killer;
    frame(b, NET_PACKET_KILL, &k, sizeof(k));
}

static void leave(DemoBuf *b, const int32_t uid) {
    const NetLeavePacket l = {.uid = uid};
    frame(b, NET_PACKET_LEAVE, &l, sizeof(l));
}

static void record(DemoBuf *out, const uint32_t t, const uint8_t kind, DemoBuf *frames) {
    demo_write_record(out, t, kind, frames->data, (uint32_t) frames->length);
    frames->length = 0;
}

// keyframe at 0 with players 1 (the suspect) and 2; the suspect dies at 20 s, respawns at 25 s (a gap under
// 10 s: one span), dies at 40 s, respawns at 60 s (a new span), leaves at 70 s; a keyframe at 50 s (dead: not
// present until its respawn); the demo ends at 90 s.
static void build(DemoBuf *out) {
    DemoBuf f = {0};
    demo_write_header(out, "{\"v\":1,\"map\":\"Sandstorm\",\"calls\":[{\"token\":\"abc\",\"suspect\":1}],\"suspects\":[2]}");
    spawn(&f, 1, 1, "Cheater");
    spawn(&f, 2, 1, "Honest");
    record(out, 0, DEMO_REC_KEYFRAME, &f);
    NetChatPacket chat;
    memset(&chat, 0, sizeof(chat));
    chat.uid = 1;
    snprintf(chat.name, sizeof(chat.name), "Cheater");
    snprintf(chat.text, sizeof(chat.text), "gg");
    frame(&f, NET_PACKET_CHAT, &chat, sizeof(chat));
    NetPlayerInfoPacket info = {.uid = 1, .account_id = 77, .level = 40, .flags = NET_PLAYER_INFO_PREMIUM | NET_PLAYER_INFO_BOT};
    snprintf(info.clan, sizeof(info.clan), "CLAN");
    frame(&f, NET_PACKET_PLAYER_INFO, &info, sizeof(info));
    record(out, 5000, DEMO_REC_STREAM, &f);
    kill(&f, 1, 2);
    record(out, 20000, DEMO_REC_STREAM, &f);
    spawn(&f, 1, 1, "Cheater");
    record(out, 25000, DEMO_REC_STREAM, &f);
    kill(&f, 1, 2);
    record(out, 40000, DEMO_REC_STREAM, &f);
    spawn(&f, 2, 1, "Honest");
    record(out, 50000, DEMO_REC_KEYFRAME, &f);
    spawn(&f, 1, 1, "Cheater");
    record(out, 60000, DEMO_REC_STREAM, &f);
    leave(&f, 1);
    record(out, 70000, DEMO_REC_STREAM, &f);
    kill(&f, 2, -1);
    record(out, 90000, DEMO_REC_STREAM, &f);
    demo_buf_free(&f);
}

static int count_packets(void *user, const uint8_t type, uint8_t *payload, const uint16_t length) {
    (void) type, (void) payload, (void) length;
    ++*(int *) user;
    return 1;
}

static int find_spawn_name(void *user, const uint8_t type, uint8_t *payload, const uint16_t length) {
    if (type == NET_PACKET_SPAWN && length >= sizeof(NetSpawnPacket)) {
        NetSpawnPacket s;
        memcpy(&s, payload, sizeof(s));
        if (s.uid == 1) snprintf(user, 32, "%s", s.name);
    }
    return 1;
}

static int find_chat_and_info(void *user, const uint8_t type, uint8_t *payload, const uint16_t length) {
    char **out = user;
    if (type == NET_PACKET_CHAT && length >= sizeof(NetChatPacket)) {
        NetChatPacket c;
        memcpy(&c, payload, sizeof(c));
        snprintf(out[0], 32, "%s", c.name);
    } else if (type == NET_PACKET_PLAYER_INFO && length >= sizeof(NetPlayerInfoPacket)) {
        NetPlayerInfoPacket i;
        memcpy(&i, payload, sizeof(i));
        snprintf(out[1], 32, "%d/%d/%u/%s", (int) i.account_id, (int) i.level, (unsigned) i.flags, i.clan);
    }
    return 1;
}

int main(void) {
    DemoBuf a = {0}, b = {0};
    build(&a);
    build(&b);
    CHECK(a.length == b.length && !memcmp(a.data, b.data, a.length)); // deterministic

    Demo d;
    char err[64] = "";
    CHECK(demo_parse(&d, a.data, a.length, err, sizeof(err)));
    CHECK(d.version == DEMO_VERSION);
    CHECK(d.count == 9);
    CHECK(d.duration_ms == 90000);
    CHECK(strstr(d.meta, "Sandstorm") != NULL);
    CHECK(demo_first_keyframe(&d) == 0);
    CHECK(demo_keyframe_at(&d, 49999) == 0);
    CHECK(demo_keyframe_at(&d, 50000) == 5);
    CHECK(demo_keyframe_at(&d, 89000) == 5);
    int packets = 0;
    CHECK(demo_frames_each(d.records[0].bytes, d.records[0].length, count_packets, &packets));
    CHECK(packets == 2);

    // round trip: writing the parsed records again gives the same file
    DemoBuf again = {0};
    demo_write_header(&again, d.meta);
    for (size_t i = 0; i < d.count; i++) demo_write_record(&again, d.records[i].t_ms, d.records[i].kind, d.records[i].bytes, d.records[i].length);
    CHECK(again.length == a.length && !memcmp(again.data, a.data, a.length));
    demo_buf_free(&again);

    // suspects
    CHECK(demo_resolve_suspect(&d, "abc") == 1);
    CHECK(demo_resolve_suspect(&d, "other") == 2);
    CHECK(demo_resolve_suspect(&d, NULL) == 2);

    DemoSegment seg[DEMO_SEGMENTS_MAX];
    size_t n = demo_suspect_segments(&d, 1, seg, DEMO_SEGMENTS_MAX);
    CHECK(n == 2);
    CHECK(n >= 1 && seg[0].start == 0 && seg[0].end == 40000);   // 0-20 s and 25-40 s merged (5 s gap)
    CHECK(n >= 2 && seg[1].start == 60000 && seg[1].end == 70000); // the 50 s keyframe: dead, so not until 60 s
    n = demo_suspect_segments(&d, -1, seg, DEMO_SEGMENTS_MAX);
    CHECK(n == 1 && seg[0].start == 0 && seg[0].end == 90000);
    n = demo_suspect_segments(&d, 99, seg, DEMO_SEGMENTS_MAX); // never in the demo: the whole of it
    CHECK(n == 1 && seg[0].end == 90000);

    const DemoSegment two[] = {{0, 40000}, {60000, 70000}};
    CHECK(demo_clamp_to_segments(two, 2, 1000) == 1000);
    CHECK(demo_clamp_to_segments(two, 2, 45000) == 40000);
    CHECK(demo_clamp_to_segments(two, 2, 55000) == 60000);
    CHECK(demo_clamp_to_segments(two, 2, 80000) == 70000);

    // hideIdentities
    demo_anonymize(&d);
    char name[32] = "";
    demo_frames_each(d.records[0].bytes, d.records[0].length, find_spawn_name, name);
    CHECK(!strcmp(name, "Player_1"));
    char chat_name[32] = "", info[32] = "";
    char *outs[2] = {chat_name, info};
    demo_frames_each(d.records[1].bytes, d.records[1].length, find_chat_and_info, outs);
    CHECK(!strcmp(chat_name, "Player_1"));
    char want[32];
    snprintf(want, sizeof(want), "0/0/%u/", (unsigned) NET_PLAYER_INFO_BOT);
    CHECK(!strcmp(info, want));
    demo_free(&d);

    // rejected files
    CHECK(!demo_parse(&d, a.data, 5, err, sizeof(err)));
    uint8_t *bad = malloc(a.length);
    memcpy(bad, a.data, a.length);
    bad[0] = 'X';
    CHECK(!demo_parse(&d, bad, a.length, err, sizeof(err)));
    CHECK(!demo_parse(&d, a.data, a.length - 3, err, sizeof(err))); // truncated record
    memcpy(bad, a.data, a.length);
    bad[DEMO_MAGIC_SIZE] = DEMO_VERSION + 1;
    CHECK(!demo_parse(&d, bad, a.length, err, sizeof(err)));
    free(bad);
    DemoBuf nokf = {0}, f = {0};
    demo_write_header(&nokf, "{}");
    leave(&f, 1);
    record(&nokf, 0, DEMO_REC_STREAM, &f);
    CHECK(!demo_parse(&d, nokf.data, nokf.length, err, sizeof(err)));
    CHECK(!strcmp(err, "demo has no keyframe"));
    DemoBuf badframes = {0};
    demo_write_header(&badframes, "{}");
    const uint8_t junk[] = {NET_PACKET_LEAVE, 0, 9, 1};
    demo_write_record(&badframes, 0, DEMO_REC_KEYFRAME, junk, sizeof(junk));
    CHECK(!demo_parse(&d, badframes.data, badframes.length, err, sizeof(err)));
    demo_buf_free(&badframes);
    demo_buf_free(&nokf);
    demo_buf_free(&f);
    demo_buf_free(&a);
    demo_buf_free(&b);

    if (g_failures) {
        fprintf(stderr, "demo_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("demo_test: ok\n");
    return 0;
}
