// The K.P.D replay player (client/include/demo_play.h, doc/v10/replays.md 2.4).
//
// Krunker v10.0.7's player (deob-10.0.7.js:201566-201963) hands every recorded socket message to the game's
// own packet handlers; here the recorded frames of a Box Office demo (shared/include/demo.h) go into
// client_net_queue_packet, like the offline sandbox's (net/offline.c), and client_tick_net applies them in
// the same frame. What Krunker's player does, and where:
//   start (:201846-201872)      the first keyframe is applied, playback waits for its world, then plays
//   injectInit (:201831-201845) an INIT (a keyframe's or a round change in the stream) holds the playhead
//                               until the map is built (game.ready, no held INIT), 20 s at most
//   tick (:201874-201905)       the playhead advances by elapsed time x speed and every frame up to it is
//                               applied; outside the suspect's segments it jumps to the next one, or ends
//   seek / _applySeek (:201918-201963)  clamp to the segments, the last keyframe at or before the target,
//                               a re-init only for another map, the players removed and the keyframe's
//                               re-added, then the state frames up to the target plus the last 3 kills
//   initGame (:226167)          a map the client has not loaded is fetched (GET /maps/<id>), and the INIT
//                               applies when it arrives; a failed fetch is dropped silently (an empty
//                               catch), so the 20 s injectInit wait runs out and playback goes on
// Box Office differences: Krunker's 33 ms setInterval is this per-frame pump (the same rule, a smoother
// step); a seek's fast-forward leaves out STATE packets older than DEMO_SEEK_STATE_MS before the target
// (they are overwritten by the later ones; Krunker applies them all), and its whitelist is written as the
// transient list below (KN has more packet types than Krunker's whitelist names). A Krunker demo's INIT
// carries a custom map inline; a Box Office INIT names its map by MAP_INFO's hash and a live client asks
// the game server for one it lacks (MAP_REQUEST), which no replay has. So every map of the demo the client
// lacks is fetched before the first keyframe goes in, the way a published map is had for Play
// (community_fetch: the brain's /api/map-data), and the demo's MAP_INFOs are pointed at that copy.
#include <demo_play.h>
#include <community_maps.h>
#include <net_ext.h>
#include <spectate.h>
#include <klog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEMO_WORLD_WAIT_S 20.0     // injectInit gives up waiting for the world after 20 s (:201843, 0x4e20)
#define DEMO_SEEK_STATE_MS 1000u   // a seek's fast-forward applies the STATEs of the last second only
#define DEMO_SEEK_KILLS 3          // _applySeek replays the last three kills (:201951)
#define DEMO_MAPS_MAX 16           // maps fetched for one demo (a 150 s window rarely holds two)

static const float k_speeds[] = {0.5f, 1.0f, 2.0f, 4.0f}; // :229783

static struct {
    int active;
    Demo demo;
    DemoSegment segments[DEMO_SEGMENTS_MAX];
    size_t segment_count;
    int32_t suspect;
    char token[48];
    char call_token[48];
    size_t next;           // frameIndex: the next record to apply
    double playhead;       // ms
    int playing;
    int speed_index;
    int waiting;           // waitingWorld
    double waiting_for;    // seconds spent waiting
    int round_started;     // onRoundStart: set when a world is reached, taken by hud_replay (menu down, follow the suspect)
    int ended_flag;
    Client *client;
    int fetching;          // map downloads still out; the first keyframe goes in when they are all back
    unsigned fetch_gen;    // a download that answers after its replay stopped (or gave up) is ignored
    // what was left at the start, given back at the exit
    int had_session;
    int had_offline;
} g_play;

int demo_play_control_packet(const uint8_t type) {
    switch (type) {
        case NET_PACKET_PING:           // "pi" (Krunker's control list: error, dc, pi, pir, nwT, lkt, load, ready, cust, frs, end)
        case NET_PACKET_PING_RESULT:    // "pir"
        case NET_PACKET_ERROR:          // "error" / "dc": the recorder's own session
        case NET_PACKET_REDIRECT:
        case NET_PACKET_AUTH_CHALLENGE:
        case NET_PACKET_MAP_REQUEST:
        case NET_PACKET_TELEPORT_ACK:
        case NET_PACKET_SOCIAL_REPLY:
        case NET_PACKET_RECON:          // our own player's reconciliation: the recorder has none
        case NET_PACKET_POSITION:
            return 1;
        default:
            return 0;
    }
}

int demo_play_transient_packet(const uint8_t type) {
    switch (type) {
        case NET_PACKET_CHAT:
        case NET_PACKET_KILL: // the last three come back after the fast-forward
        case NET_PACKET_HIT:
        case NET_PACKET_SHOT:
        case NET_PACKET_PROJECTILE:
        case NET_PACKET_PROJECTILE_END:
        case NET_PACKET_EXPLOSION:
        case NET_PACKET_VOICE:
        case NET_PACKET_SOUND:
        case NET_PACKET_JUMP_SCARE:
        case NET_PACKET_POPUP:
        case NET_PACKET_NOTIFY:
        case NET_PACKET_GAME_MESSAGE:
        case NET_PACKET_MEDAL:
        case NET_PACKET_KILL_MEDALS:
        case NET_PACKET_KILL_STREAK:
        case NET_PACKET_ASSIST:
        case NET_PACKET_SPRAY:
        case NET_PACKET_ACTION:
            return 1;
        default:
            return 0;
    }
}

// ---------------------------------------------------------------- feeding

typedef struct {
    Client *client;
    int skip_map;       // a same-map seek: the keyframe's INIT and map packets are left out
    int fast;           // a seek's fast-forward: transient packets left out
    uint32_t t;         // the record's time (fast-forward: STATEs before the cut are left out)
    uint32_t state_cut;
    int saw_init;
} Feed;

static void feed_one(Client *client, const uint8_t type, const uint8_t *payload, const uint16_t length) {
    if (length > NET_MAX_PAYLOAD) return; // no recorded packet is (net_send_packet's cap)
    NetPacket packet;
    packet.type = type;
    packet.length = length;
    if (length) memcpy(packet.payload, payload, length);
    client_net_queue_packet(client, &packet);
}

static int feed_packet(void *user, const uint8_t type, uint8_t *payload, const uint16_t length) {
    Feed *f = user;
    if (demo_play_control_packet(type)) return 1;
    if (f->skip_map && (type == NET_PACKET_INIT || type == NET_PACKET_MAP_INFO || type == NET_PACKET_MAP_DATA ||
                        type == NET_PACKET_BILLBOARD || type == NET_PACKET_BILLBOARD_IMAGE))
        return 1;
    if (f->fast && demo_play_transient_packet(type)) return 1;
    if (f->fast && type == NET_PACKET_STATE && f->t < f->state_cut) return 1;
    if (type == NET_PACKET_INIT) f->saw_init = 1;
    feed_one(f->client, type, payload, length);
    return 1;
}

// One record into the queue; 1 when it carried an INIT (injectInit: the playhead waits for the world)
static int feed_record(Client *client, const DemoRecord *r, const int skip_map, const int fast, const uint32_t state_cut) {
    Feed f = {.client = client, .skip_map = skip_map, .fast = fast, .t = r->t_ms, .state_cut = state_cut};
    demo_frames_each(r->bytes, r->length, feed_packet, &f);
    return f.saw_init;
}

static void wait_world(void) {
    g_play.waiting = 1;
    g_play.waiting_for = 0.0;
    g_play.round_started = 0;
}

// The record's INIT names the map (by MAP_INFO's hash); a keyframe without one keeps the current map
static int record_map_hash(const DemoRecord *r, uint32_t *hash) {
    int found = 0;
    for (uint32_t at = 0; at + 3 <= r->length;) {
        const uint8_t type = r->bytes[at];
        const uint16_t length = (uint16_t) ((r->bytes[at + 1] << 8) | r->bytes[at + 2]);
        if (at + 3u + length > r->length) break;
        if (type == NET_PACKET_MAP_INFO && length >= sizeof(NetMapInfoPacket)) {
            NetMapInfoPacket info;
            memcpy(&info, r->bytes + at + 3, sizeof(info));
            *hash = info.hash;
            found = 1;
        }
        at += 3u + length;
    }
    return found;
}

static uint32_t g_current_map_hash; // currentMapId: the hash of the map the last INIT applied
static int g_current_map_known;

// ---------------------------------------------------------------- the demo's maps

// What client_map_prepare_init finds an INIT's map in: the game's map list, the maps servers streamed
static int client_has_map(const Client *client, const uint32_t hash) {
    for (size_t i = 0; i < client->game.map_count; i++)
        if (client->game.maps[i] && net_map_hash(client->game.maps[i], NULL, NULL) == hash) return 1;
    for (size_t i = 0; i < client->net_map_cache_count; i++)
        if (client->net_map_cache[i] && net_map_hash(client->net_map_cache[i], NULL, NULL) == hash) return 1;
    return 0;
}

// start's injectInit: the first keyframe goes in (its INIT), the world wait already runs
static void begin_playback(Client *client) {
    const DemoRecord *kf = &g_play.demo.records[g_play.next - 1];
    g_current_map_known = record_map_hash(kf, &g_current_map_hash);
    feed_record(client, kf, 0, 0, 0);
}

static void map_fetch_settle(void) {
    if (g_play.active && g_play.fetching > 0 && --g_play.fetching == 0) begin_playback(g_play.client);
}

typedef struct {
    unsigned gen;
    uint32_t hash;
    char name[64];
} MapFetch;

static void on_map_fetched(void *user, const cJSON *json, const char *error) {
    MapFetch *f = user;
    if (f->gen == g_play.fetch_gen && g_play.active && g_play.fetching > 0) {
        Client *client = g_play.client;
        // the map list keeps it (and the session cache never evicts a listed map); the demo's MAP_INFOs then
        // name its hash, which may differ from the recording server's copy (the server sanitises its own)
        if (json && community_slot_of(&client->game.maps, &client->game.map_count, json) >= 0) {
            demo_retarget_map(&g_play.demo, f->hash, net_map_hash(json, NULL, NULL));
            printf("[replay] map %s fetched\n", f->name);
        } else {
            // initGame's catch is empty: the INIT waits for a map that never comes, injectInit's 20 s run out
            fprintf(stderr, "[replay] map %s cannot be fetched: %s\n", f->name, error ? error : "no map");
        }
        map_fetch_settle();
    }
    free(f);
}

static void fetch_missing_maps(Client *client) {
    g_play.fetch_gen++;
    g_play.fetching = 1; // held while the requests go out: a cached map answers at once
    DemoMap maps[DEMO_MAPS_MAX];
    const size_t count = demo_maps(&g_play.demo, maps, DEMO_MAPS_MAX);
    for (size_t i = 0; i < count; i++) {
        if (client_has_map(client, maps[i].hash)) continue;
        MapFetch *f = calloc(1, sizeof(*f));
        if (!f) continue;
        f->gen = g_play.fetch_gen;
        f->hash = maps[i].hash;
        snprintf(f->name, sizeof(f->name), "%s", maps[i].name);
        g_play.fetching++;
        printf("[replay] fetching map %s\n", f->name);
        community_fetch(f->name, on_map_fetched, f);
    }
    map_fetch_settle();
}

// ---------------------------------------------------------------- start / stop

int demo_play_active(void) { return g_play.active; }
int demo_play_waiting(void) { return g_play.active && g_play.waiting; }
int demo_play_playing(void) { return g_play.active && g_play.playing; }
float demo_play_speed(void) { return k_speeds[g_play.speed_index]; }
uint32_t demo_play_playhead_ms(void) {
    if (!g_play.active) return 0;
    const double t = g_play.playhead < 0.0 ? 0.0 : g_play.playhead;
    return t > g_play.demo.duration_ms ? g_play.demo.duration_ms : (uint32_t) t;
}
uint32_t demo_play_duration_ms(void) { return g_play.active ? g_play.demo.duration_ms : 0; }
int demo_play_suspect_uid(void) { return g_play.active ? g_play.suspect : -1; }
const char *demo_play_token(void) { return g_play.active ? g_play.token : ""; }
const char *demo_play_call_token(void) { return g_play.active ? g_play.call_token : ""; }
const char *demo_play_meta(void) { return g_play.active && g_play.demo.meta ? g_play.demo.meta : ""; }
const DemoSegment *demo_play_segments(size_t *count) {
    if (count) *count = g_play.active ? g_play.segment_count : 0;
    return g_play.segments;
}

int demo_play_take_round_start(void) {
    const int v = g_play.active && g_play.round_started;
    g_play.round_started = 0;
    return v;
}

int demo_play_take_ended(void) {
    const int v = g_play.ended_flag;
    g_play.ended_flag = 0;
    return v;
}

void demo_play_set_playing(const int playing) {
    if (!g_play.active) return;
    // setPlaying(true) after the end starts over from the first segment (Krunker's stays at the end: a
    // play click there does nothing until a seek)
    g_play.playing = playing ? 1 : 0;
}

void demo_play_cycle_speed(void) {
    g_play.speed_index = (g_play.speed_index + 1) % (int) (sizeof(k_speeds) / sizeof(k_speeds[0]));
}

int demo_play_start(Client *client, const uint8_t *data, const size_t length, const char *demo_token, const char *call_token,
                    char *err, const size_t err_size) {
    if (!client) return 0;
    Demo demo;
    char why[96] = "";
    if (!demo_parse(&demo, data, length, why, sizeof(why))) {
        fprintf(stderr, "[replay] invalid demo: %s\n", why);
        if (err && err_size) snprintf(err, err_size, "Invalid Demo File"); // watchDemo's popup (:229714)
        return 0;
    }
    if (g_play.active) demo_play_stop(client);

    // window.isReplay; the live socket closed (:229716-229722)
    g_play.had_offline = client->offline;
    g_play.had_session = !client->offline && (client->net_connected || client->net_thread_running);
    client->replay = 1; // before the release: its session's close is the replay's, not a server switch (events.c)
    client_net_release(client);
    client_leave_game(client);

    g_play.demo = demo;
    g_play.active = 1;
    demo_anonymize(&g_play.demo); // hideIdentities at load (:201752-201782)
    g_play.suspect = demo_resolve_suspect(&g_play.demo, call_token);
    g_play.segment_count = demo_suspect_segments(&g_play.demo, g_play.suspect, g_play.segments, DEMO_SEGMENTS_MAX);
    snprintf(g_play.token, sizeof(g_play.token), "%s", demo_token ? demo_token : "");
    snprintf(g_play.call_token, sizeof(g_play.call_token), "%s", call_token ? call_token : "");
    g_play.speed_index = 1;
    g_play.ended_flag = 0;
    client->replay = 1;
    client->net_connected = 0;
    client->net_ping_ms = -1;

    // start: the first keyframe once the demo's maps are here, then play once its world is there
    const int first = demo_first_keyframe(&g_play.demo);
    g_play.next = (size_t) first + 1;
    g_play.playhead = g_play.demo.records[first].t_ms;
    g_play.playing = 1;
    g_play.client = client;
    wait_world();
    printf("[replay] demo %s: %zu records, %u ms, suspect %d, %zu segment(s)\n", g_play.token[0] ? g_play.token : "(file)",
           g_play.demo.count, g_play.demo.duration_ms, g_play.suspect, g_play.segment_count);
    fetch_missing_maps(client);
    return 1;
}

int demo_play_start_file(Client *client, const char *path, const char *call_token, char *err, const size_t err_size) {
    FILE *file = path ? fopen(path, "rb") : NULL;
    if (!file) {
        if (err && err_size) snprintf(err, err_size, "Demo Download Failed");
        fprintf(stderr, "[replay] cannot open %s\n", path ? path : "(null)");
        return 0;
    }
    uint8_t *data = NULL;
    size_t length = 0;
    if (fseek(file, 0, SEEK_END) == 0) {
        const long size = ftell(file);
        if (size > 0 && (unsigned long) size <= DEMO_MAX_BYTES) {
            data = malloc((size_t) size);
            rewind(file);
            if (data && fread(data, 1, (size_t) size, file) == (size_t) size) length = (size_t) size;
        }
    }
    fclose(file);
    // the token: the file's name without its .kndemo
    char token[48] = "";
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(token, sizeof(token), "%s", base);
    char *dot = strstr(token, ".kndemo");
    if (dot) *dot = 0;
    const int ok = length && demo_play_start(client, data, length, token, call_token, err, err_size);
    if (!length && err && err_size) snprintf(err, err_size, "Invalid Demo File");
    free(data);
    return ok;
}

void demo_play_stop(Client *client) {
    if (!g_play.active) return;
    g_play.active = 0;
    g_play.fetching = 0;
    g_play.fetch_gen++;
    demo_free(&g_play.demo);
    memset(&g_play.demo, 0, sizeof(g_play.demo));
    client->replay = 0;
    // the replay's queued packets go with it
    pthread_mutex_lock(&client->net_lock);
    free(client->net_packets);
    client->net_packets = NULL;
    client->net_packet_count = 0;
    pthread_mutex_unlock(&client->net_lock);
    client_leave_game(client);
    if (client->map_loaded) {
        client_unload_map(client);
        client->map_loaded = 0;
    }
    client->game.ready = 0;
    client->in_game = 0;
    printf("[replay] stopped\n");
    // location.reload(): the game the replay replaced comes back
    if (g_play.had_offline) local_game_start(client);
    else if (g_play.had_session) client_net_switch_server(client);
}

// ---------------------------------------------------------------- seek

static void apply_seek(Client *client, const int kf_index, const uint32_t target, const int same_map) {
    const DemoRecord *kf = &g_play.demo.records[kf_index];
    if (same_map) {
        // removePlayer for every player, then the keyframe's roster (:201943-201950)
        client_clear_players(client);
        client->in_game = 0;
    }
    feed_record(client, kf, same_map, 0, 0);
    const uint32_t cut = target > DEMO_SEEK_STATE_MS ? target - DEMO_SEEK_STATE_MS : 0;
    // the state frames up to the target, the last three kills after them (:201951-201956)
    const DemoRecord *kills[DEMO_SEEK_KILLS];
    int kill_count = 0;
    size_t i = (size_t) kf_index + 1;
    for (; i < g_play.demo.count && g_play.demo.records[i].t_ms <= target; i++) {
        const DemoRecord *r = &g_play.demo.records[i];
        if (r->kind == DEMO_REC_KEYFRAME) continue; // a later keyframe restates what the stream has
        feed_record(client, r, 0, 1, cut);
        for (uint32_t at = 0; at + 3 <= r->length;) { // its KILL frames, the last three kept
            const uint16_t length = (uint16_t) ((r->bytes[at + 1] << 8) | r->bytes[at + 2]);
            if (at + 3u + length > r->length) break;
            if (r->bytes[at] == NET_PACKET_KILL) {
                if (kill_count == DEMO_SEEK_KILLS) {
                    memmove(kills, kills + 1, sizeof(kills[0]) * (DEMO_SEEK_KILLS - 1));
                    kill_count--;
                }
                kills[kill_count++] = r;
                break; // a record once (its kills are the tick's)
            }
            at += 3u + length;
        }
    }
    for (int k = 0; k < kill_count; k++) {
        const DemoRecord *r = kills[k];
        for (uint32_t at = 0; at + 3 <= r->length;) {
            const uint8_t type = r->bytes[at];
            const uint16_t length = (uint16_t) ((r->bytes[at + 1] << 8) | r->bytes[at + 2]);
            if (at + 3u + length > r->length) break;
            if (type == NET_PACKET_KILL) feed_one(client, type, r->bytes + at + 3, length);
            at += 3u + length;
        }
    }
    g_play.playhead = target;
    g_play.next = i;
    g_play.ended_flag = 1;
    // no onRoundStart for a same-map seek (_applySeek, deob-10.0.7.js:201934-201954): the menu and popups stay, and the
    // camera finds the suspect again at the next 1.5 s re-follow (:229810-229812)
}

void demo_play_seek(Client *client, uint32_t t_ms) {
    if (!g_play.active || g_play.waiting) return; // seek does nothing while waitingWorld (:201920)
    if (t_ms > g_play.demo.duration_ms) t_ms = g_play.demo.duration_ms;
    t_ms = demo_clamp_to_segments(g_play.segments, g_play.segment_count, t_ms);
    const int kf = demo_keyframe_at(&g_play.demo, t_ms);
    if (kf < 0) return;
    uint32_t hash = 0;
    const int named = record_map_hash(&g_play.demo.records[kf], &hash);
    const int same_map = !named || (g_current_map_known && hash == g_current_map_hash);
    if (!same_map) {
        g_current_map_hash = hash;
        g_current_map_known = 1;
        wait_world(); // injectInit, then _applySeek (the whole keyframe goes in: its INIT re-inits)
    }
    apply_seek(client, kf, t_ms, same_map);
}

// ---------------------------------------------------------------- tick

static int in_segment(const double t, double *next_start) {
    *next_start = -1.0;
    if (!g_play.segment_count) return 1;
    for (size_t i = 0; i < g_play.segment_count; i++) {
        if (t >= g_play.segments[i].start && t <= g_play.segments[i].end) return 1;
        if (g_play.segments[i].start > t) {
            *next_start = g_play.segments[i].start;
            return 0;
        }
    }
    return 0;
}

static void end_playback(void) {
    g_play.playing = 0;
    g_play.ended_flag = 1;
}

void demo_play_pump(Client *client, const float delta) {
    if (!g_play.active) return;
    if (g_play.waiting) {
        g_play.waiting_for += delta > 0.0f ? delta : 0.0f;
        if (g_play.fetching > 0) {
            if (g_play.waiting_for < DEMO_WORLD_WAIT_S) return;
            // the 20 s cover the map's download too (initGame fetches inside injectInit's wait)
            fprintf(stderr, "[replay] the demo's map did not arrive in %.0f s\n", DEMO_WORLD_WAIT_S);
            g_play.fetch_gen++;
            g_play.fetching = 0;
            begin_playback(client);
        }
        // the world: the INIT applied (no held INIT, no map download) and the map built
        const int ready = client->game.ready && !client->net_map_wait && !client_init_pending(client);
        if (!ready && g_play.waiting_for < DEMO_WORLD_WAIT_S) return;
        if (!ready) fprintf(stderr, "[replay] the demo's map did not load in %.0f s, playing on\n", DEMO_WORLD_WAIT_S);
        else {
            const int index = client->game.current_map_index;
            const cJSON *map = index >= 0 && (size_t) index < client->game.map_count ? client->game.maps[index] : NULL;
            const cJSON *name = cJSON_GetObjectItemCaseSensitive(map, "name");
            printf("[replay] world: %s\n", cJSON_IsString(name) ? name->valuestring : "(unnamed map)");
        }
        g_play.waiting = 0;
        g_play.round_started = 1; // onRoundStart (:229724-229731)
        return; // the first frame of the world: the roster is applied; play from the next one
    }
    if (!g_play.playing) return;

    const double before = g_play.playhead;
    g_play.playhead += (double) (delta > 0.0f ? delta : 0.0f) * 1000.0 * k_speeds[g_play.speed_index];
    if ((uint32_t) (before / 1000.0) != (uint32_t) (g_play.playhead / 1000.0))
        if (getenv("KRUNKNATIVE_REPLAY_LOG")) printf("[replay] %.0f / %u ms, record %zu of %zu\n", g_play.playhead, g_play.demo.duration_ms, g_play.next, g_play.demo.count);
    double next_start;
    if (!in_segment(g_play.playhead, &next_start)) {
        if (next_start >= 0.0) demo_play_seek(client, (uint32_t) next_start);
        else end_playback();
        return;
    }
    while (g_play.next < g_play.demo.count && g_play.demo.records[g_play.next].t_ms <= g_play.playhead) {
        const DemoRecord *r = &g_play.demo.records[g_play.next++];
        if (r->kind == DEMO_REC_KEYFRAME) continue; // only seeks start from keyframes
        if (feed_record(client, r, 0, 0, 0)) { // a round change: wait for its world
            uint32_t hash;
            if (record_map_hash(r, &hash)) {
                g_current_map_hash = hash;
                g_current_map_known = 1;
            }
            wait_world();
            break;
        }
    }
    if (g_play.next >= g_play.demo.count && !g_play.waiting) end_playback();
}
